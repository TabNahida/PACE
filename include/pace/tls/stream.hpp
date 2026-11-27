#pragma once

#include <pace/net/tcp.hpp>

#ifdef PACE_HAS_TLS

#include <array>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pace::tls
{
class stream;
} // namespace pace::tls

namespace pace::tls
{
class tls_driver;

template <class Transport>
concept tls_closeable = requires(Transport& transport) {
    { transport.close() } noexcept -> std::same_as<void>;
};

template <class Transport>
concept tls_resettable = requires(Transport& transport) {
    { transport.finish_write() } -> std::same_as<void>;
    { transport.reset() } noexcept -> std::same_as<void>;
};

template <class Transport>
concept tls_byte_stream =
    (tls_closeable<Transport> || tls_resettable<Transport>)
    && requires(Transport& transport, const Transport& owner, std::span<char> input, std::string_view output) {
           { owner.context() } noexcept -> std::same_as<io_context&>;
           { transport.read_some(input) } -> std::same_as<task<std::size_t>>;
           { transport.write_all(output) } -> std::same_as<task<void>>;
       };

class tls_transport
{
public:
    virtual ~tls_transport() = default;
    virtual io_context& context() const noexcept = 0;
    virtual bool is_open() const noexcept = 0;
    virtual tls::stream* nested_stream() noexcept = 0;
    virtual bool supports_concurrent_io() const noexcept = 0;
    virtual task<std::size_t> read_some(std::span<char>) = 0;
    virtual task<void> write_all(std::string_view) = 0;
    virtual task<void> shutdown(std::chrono::milliseconds) = 0;
    virtual void close() noexcept = 0;
};

template <tls_byte_stream Transport>
class tls_transport_model final : public tls_transport
{
public:
    explicit tls_transport_model(Transport transport)
        : transport_(std::move(transport))
    {
    }

    tls_transport_model(const tls_transport_model&) = delete;
    tls_transport_model& operator=(const tls_transport_model&) = delete;

    ~tls_transport_model() override
    {
        close();
    }

    io_context& context() const noexcept override
    {
        return transport_.context();
    }

    bool is_open() const noexcept override
    {
        if constexpr (requires {
                          { transport_.is_open() } noexcept -> std::same_as<bool>;
                      })
        {
            return open_ && transport_.is_open();
        }
        return open_;
    }

    tls::stream* nested_stream() noexcept override
    {
        if constexpr (std::same_as<Transport, tls::stream>)
        {
            return &transport_;
        }
        else
        {
            return nullptr;
        }
    }

    bool supports_concurrent_io() const noexcept override
    {
        if constexpr (requires {
                          { transport_.supports_concurrent_io() } noexcept -> std::same_as<bool>;
                      })
        {
            return transport_.supports_concurrent_io();
        }
        else
        {
            return true;
        }
    }

    task<std::size_t> read_some(std::span<char> buffer) override
    {
        return transport_.read_some(buffer);
    }

    task<void> write_all(std::string_view bytes) override
    {
        return transport_.write_all(bytes);
    }

    task<void> shutdown(std::chrono::milliseconds timeout) override
    {
        if constexpr (tls_closeable<Transport> && requires {
                          { transport_.shutdown(timeout) } -> std::same_as<task<void>>;
                      })
        {
            co_await transport_.shutdown(timeout);
            close();
        }
        else if constexpr (tls_resettable<Transport>)
        {
            transport_.finish_write();
            // Dropping a QUIC stream before the peer's FIN cancels its send side.
            std::array<char, 4096> discard{};
            while (co_await transport_.read_some(discard))
            {
                co_await context().yield();
            }
            open_ = false;
        }
        else
        {
            close();
        }
        co_return;
    }

    void close() noexcept override
    {
        if (!std::exchange(open_, false))
        {
            return;
        }
        if constexpr (tls_closeable<Transport>)
        {
            transport_.close();
        }
        else
        {
            transport_.reset();
        }
    }

    Transport release()
    {
        Transport result(std::move(transport_));
        open_ = false;
        return result;
    }

private:
    Transport transport_;
    bool open_ = true;
};
} // namespace pace::tls

namespace pace::tls
{
enum class role
{
    client,
    server
};
enum class trust_source
{
    system,
    application
};
enum class sni_policy
{
    use_default,
    reject
};
enum class pin_type
{
    leaf_certificate,
    subject_public_key_info
};
using sha256_digest = std::array<std::byte, 32>;
// Peer-supplied DER certificates in wire order, leaf first. The view and its
// contents are borrowed for the call. Return true to accept the chain.
using verification_callback = std::function<bool(std::span<const std::string>)>;

struct client_hello_key_share
{
    std::uint16_t group = 0;
    std::string_view key_exchange;
};

// Wire order and wire IDs. An absent SNI, ALPN or supported_versions extension
// yields an empty field. All views are borrowed until the callback returns.
struct client_hello
{
    std::string_view server_name;
    std::vector<std::string_view> alpn_protocols;
    std::vector<std::uint16_t> supported_versions;
    std::vector<std::uint16_t> cipher_suites;
    std::vector<client_hello_key_share> key_shares;
    std::uint16_t legacy_version = 0;
    std::string_view session_id;
    // The exact handshake message, including its four-byte handshake header.
    std::string_view raw_message;
};

class client_hello_action;
using client_hello_callback = std::function<client_hello_action(const client_hello&)>;

// Configuration freezes on first stream construction, registration as an SNI
// target or selection by a ClientHello callback. Subsequent setters throw
// logic_error. Streams retain shared ownership of their configuration and
// routing targets. Clients default to CA and name verification.
class context
{
public:
    // application starts with no trusted roots and never loads default paths.
    explicit context(role mode = role::client, trust_source trust = trust_source::system);
    ~context();
    context(context&&) noexcept;
    context& operator=(context&&) noexcept;
    context(const context&) = delete;
    context& operator=(const context&) = delete;

    void load_verify_file(const std::string& pem_file);
    // Copies one or more PEM CERTIFICATE blocks into the owned trust store.
    // No filesystem access; the input may be released immediately after return.
    void load_verify_pem(std::string_view certificates);
    // Client-only alternatives to CA verification. The pin hashes the DER
    // encoding of the selected leaf object. Both modes check any supplied peer name.
    void set_verify_pin(pin_type type, sha256_digest digest);
    // Runs synchronously on the owning reactor. May run concurrently across
    // reactors sharing this context. Returning false or throwing rejects the peer.
    void set_verify_callback(verification_callback verifier);
    // Disables certificate and name verification for testing and interoperability.
    void insecure_skip_verify();
    void use_certificate_chain(const std::string& certificate_chain_file, const std::string& private_key_file);
    // Server preference order; an offered list without overlap fails negotiation.
    // An empty list disables ALPN. Clients must check the negotiated protocol.
    void set_alpn_protocols(const std::vector<std::string>& protocols);
    // Exact ASCII DNS names, case-insensitive; no wildcards or IP literals.
    // Registration freezes and retains the server target. Targets cannot contain
    // their own SNI mappings. A selected target's failure never uses the default.
    void add_server_name(std::string name, const context& configuration);
    // Missing and unrecognized SNI independently use the default certificate or
    // fail this handshake. Both default to use_default. Server contexts only.
    void set_server_name_policy(sni_policy missing, sni_policy unrecognized);
    // Server only. Invoked once for the first ClientHello, before certificate
    // selection. The callback runs on the reactor and may run concurrently
    // across reactors sharing the context. Throwing aborts the handshake.
    void set_client_hello_callback(client_hello_callback callback);

private:
    struct implementation;
    std::shared_ptr<implementation> impl_;
    friend class stream;
    friend class client_hello_action;
};

class client_hello_action
{
public:
    // Continue with this context's ordinary SNI and certificate selection.
    static client_hello_action proceed() noexcept
    {
        return {};
    }

    // Select and retain a server context immediately. Its configuration freezes.
    static client_hello_action select(const context& configuration);

    // Send a fatal handshake alert.
    static client_hello_action reject() noexcept
    {
        return client_hello_action(kind::reject);
    }

    // Pause before writing anything, for handshake_with_handoff().
    static client_hello_action handoff() noexcept
    {
        return client_hello_action(kind::handoff);
    }

private:
    enum class kind
    {
        proceed,
        select,
        reject,
        handoff
    };

    explicit client_hello_action(kind value) noexcept
        : kind_(value)
    {
    }

    client_hello_action() = default;
    kind kind_ = kind::proceed;
    std::shared_ptr<context::implementation> configuration_;
    friend class stream;
};

template <class Transport>
struct client_hello_handoff
{
    Transport transport;
    // Every byte already read in wire order, including the constructor prefix
    // and any bytes read ahead after the ClientHello.
    std::string pre_read;
};

// Move-only TLS over an owned byte stream. Exactly one asynchronous operation may
// be active at a time (including reads vs writes). Move only while idle; preserve
// this object's address and borrowed buffers until each operation finishes.
class stream
{
public:
    stream(io_context& io, const tls::context& configuration);
    stream(tcp::stream transport, const tls::context& configuration);
    // Copies TLS bytes already consumed from this TCP stream and replays them
    // before reading from the socket. The prefix may end anywhere in a record.
    stream(tcp::stream transport, const tls::context& configuration, std::string_view pre_read);

    // The transport must already be connected and ready for bidirectional I/O.
    // Its close/reset must cancel pending operations on its owning reactor.
    // Shutdown may overlap a transport read and write. Adapters with serialized
    // I/O can declare supports_concurrent_io() const noexcept returning false.
    template <tls_byte_stream Transport>
    requires(!std::same_as<Transport, tcp::stream>)
    stream(Transport transport, const tls::context& configuration, std::string_view pre_read = {})
        : stream(std::make_unique<tls_transport_model<Transport>>(std::move(transport)), configuration, pre_read)
    {
    }

    ~stream();
    stream(stream&&) noexcept;
    stream& operator=(stream&&) noexcept;
    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;

    // connect is available on TCP-backed streams. CA verification requires a DNS
    // name or IP address. The default SNI is
    // derived from a DNS peer name. An explicit empty server_name suppresses SNI.
    task<void> connect(tcp::endpoint endpoint, std::string peer_name);
    task<void> connect(tcp::endpoint endpoint, std::string peer_name, std::string server_name);
    task<void> handshake(std::string peer_name = {});
    task<void> handshake(std::string peer_name, std::string server_name);

    // Server only. Empty on a completed TLS handshake. On handoff this stream
    // becomes moved-from and the caller owns the original, still-open transport
    // and its pre-read bytes. Transport must match the type passed at construction.
    template <tls_byte_stream Transport = tcp::stream>
    task<std::optional<client_hello_handoff<Transport>>> handshake_with_handoff()
    {
        using model_type = tls_transport_model<Transport>;
        if constexpr (std::same_as<Transport, tcp::stream>)
        {
            require_tcp_handoff();
        }
        else if (!dynamic_cast<model_type*>(generic_transport()))
        {
            throw std::logic_error("TLS handoff requires the original transport type");
        }
        co_await handshake_impl({}, std::nullopt, true);
        if (!handoff_requested())
        {
            co_return std::nullopt;
        }
        if constexpr (std::same_as<Transport, tcp::stream>)
        {
            co_return release_tcp();
        }
        else
        {
            auto [transport, prefix] = release_generic();
            auto& model = static_cast<model_type&>(*transport);
            co_return client_hello_handoff<Transport>{model.release(), std::move(prefix)};
        }
    }

    task<std::size_t> read_some(std::span<char> buffer);
    task<void> write_all(std::string_view bytes);

    task<std::size_t> read_some(std::span<std::byte> buffer)
    {
        return read_some(std::span<char>(reinterpret_cast<char*>(buffer.data()), buffer.size()));
    }

    task<void> write_all(std::span<const std::byte> bytes)
    {
        return write_all(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }

    // Exchange close_notify cooperatively, closing even on cancellation/error or
    // timeout. Expiring this bound closes normally, without promising peer receipt.
    task<void> shutdown(std::chrono::milliseconds timeout = std::chrono::milliseconds(100));

    task<void> close_gracefully(std::chrono::milliseconds timeout = std::chrono::milliseconds(100))
    {
        return shutdown(timeout);
    }

    void close() noexcept;
    void set_no_delay(bool enabled = true);
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] io_context& context() const noexcept;
    [[nodiscard]] std::string selected_protocol() const;

private:
    struct implementation;
    std::unique_ptr<implementation> impl_;
    stream(std::unique_ptr<tls_transport> transport, const tls::context& configuration, std::string_view pre_read);
    friend class tls_driver;
    friend struct tls::context::implementation;
    task<void> connect_impl(tcp::endpoint endpoint, std::string peer_name, std::optional<std::string> server_name);
    task<void> handshake_impl(std::string peer_name, std::optional<std::string> server_name, bool allow_handoff);
    task<bool> perform_handshake();
    bool handoff_requested() const noexcept;
    void require_tcp_handoff() const;
    tls_transport* generic_transport() const noexcept;
    client_hello_handoff<tcp::stream> release_tcp();
    client_hello_handoff<std::unique_ptr<tls_transport>> release_generic();
};

// Complete a handshake using the supplied verification policy. Custom
// configuration is borrowed through task completion; streams retain its state.
inline task<stream> connect(io_context& io, const context& configuration, tcp::endpoint peer, std::string expected_name)
{
    stream result(io, configuration);
    co_await result.connect(std::move(peer), std::move(expected_name));
    co_return result;
}

inline task<stream> connect(const context& configuration, tcp::endpoint peer, std::string expected_name)
{
    co_return co_await connect(this_context(), configuration, std::move(peer), std::move(expected_name));
}

// System trust for a client that needs no custom CA or ALPN configuration.
inline task<stream> connect(io_context& io, tcp::endpoint peer, std::string expected_name)
{
    context configuration;
    co_return co_await connect(io, configuration, std::move(peer), std::move(expected_name));
}

inline task<stream> connect(tcp::endpoint peer, std::string expected_name)
{
    co_return co_await connect(this_context(), std::move(peer), std::move(expected_name));
}

inline task<stream> accept(tcp::stream transport, const context& configuration)
{
    stream result(std::move(transport), configuration);
    co_await result.handshake();
    co_return result;
}
} // namespace pace::tls
#endif // PACE_HAS_TLS
