#pragma once

#include <pace/tcp.hpp>

#ifdef PACE_HAS_TLS

#include <array>
#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pace::detail {
class tls_driver;

template<class Transport>
concept tls_closeable = requires(Transport& transport) {
    { transport.close() } noexcept -> std::same_as<void>;
};

template<class Transport>
concept tls_resettable = requires(Transport& transport) {
    { transport.finish_write() } -> std::same_as<void>;
    { transport.reset() } noexcept -> std::same_as<void>;
};

template<class Transport>
concept tls_byte_stream = (tls_closeable<Transport> || tls_resettable<Transport>) &&
    requires(Transport& transport, const Transport& owner, std::span<char> input, std::string_view output) {
        { owner.context() } noexcept -> std::same_as<io_context&>;
        { transport.read_some(input) } -> std::same_as<task<std::size_t>>;
        { transport.write_all(output) } -> std::same_as<task<void>>;
    };

class tls_transport {
public:
    virtual ~tls_transport() = default;
    virtual io_context& context() const noexcept = 0;
    virtual bool is_open() const noexcept = 0;
    virtual task<std::size_t> read_some(std::span<char>) = 0;
    virtual task<void> write_all(std::string_view) = 0;
    virtual task<void> shutdown(std::chrono::milliseconds) = 0;
    virtual void close() noexcept = 0;
};

template<tls_byte_stream Transport>
class tls_transport_model final : public tls_transport {
public:
    explicit tls_transport_model(Transport transport) : transport_(std::move(transport)) {}
    ~tls_transport_model() override { close(); }
    io_context& context() const noexcept override { return transport_.context(); }
    bool is_open() const noexcept override {
        if constexpr (requires { { transport_.is_open() } noexcept -> std::same_as<bool>; })
            return open_ && transport_.is_open();
        return open_;
    }
    task<std::size_t> read_some(std::span<char> buffer) override { return transport_.read_some(buffer); }
    task<void> write_all(std::string_view bytes) override { return transport_.write_all(bytes); }
    task<void> shutdown(std::chrono::milliseconds timeout) override {
        if constexpr (requires { { transport_.shutdown(timeout) } -> std::same_as<task<void>>; }) {
            co_await transport_.shutdown(timeout);
        } else if constexpr (tls_resettable<Transport>) {
            transport_.finish_write();
            // Dropping a QUIC stream before the peer's FIN cancels its send side.
            std::array<char, 4096> discard{};
            while (co_await transport_.read_some(discard)) co_await context().yield();
        } else close();
        open_ = false;
        co_return;
    }
    void close() noexcept override {
        if (!std::exchange(open_, false)) return;
        if constexpr (tls_closeable<Transport>) transport_.close();
        else transport_.reset();
    }
private:
    Transport transport_;
    bool open_ = true;
};
} // namespace pace::detail

namespace pace::tls {

enum class role { client, server };
enum class trust_source { system, application };
enum class sni_policy { use_default, reject };
enum class pin_type { leaf_certificate, subject_public_key_info };
using sha256_digest = std::array<std::byte, 32>;
// Peer-supplied DER certificates in wire order, leaf first. The view and its
// contents are borrowed for the call. Return true to accept the chain.
using verification_callback = std::function<bool(std::span<const std::string>)>;

// Configuration freezes on first stream construction or registration as an SNI
// target; subsequent setters throw logic_error. Streams retain shared ownership
// of their configuration and every SNI target. Clients default to CA and name verification.
class context {
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
    void use_certificate_chain(const std::string& certificate_chain_file,
                               const std::string& private_key_file);
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
private:
    struct implementation;
    std::shared_ptr<implementation> impl_;
    friend class stream;
};

// Move-only TLS over an owned byte stream. Exactly one asynchronous operation may
// be active at a time (including reads vs writes). Move only while idle; preserve
// this object's address and borrowed buffers until each operation finishes.
class stream {
public:
    stream(io_context& io, const tls::context& configuration);
    stream(tcp::stream transport, const tls::context& configuration);
    // Copies TLS bytes already consumed from this TCP stream and replays them
    // before reading from the socket. The prefix may end anywhere in a record.
    stream(tcp::stream transport, const tls::context& configuration, std::string_view pre_read);
    // The transport must already be connected and ready for bidirectional I/O.
    // Its close/reset must cancel pending operations on its owning reactor.
    template<pace::detail::tls_byte_stream Transport> requires (!std::same_as<Transport, tcp::stream>)
    stream(Transport transport, const tls::context& configuration, std::string_view pre_read = {})
        : stream(std::make_unique<pace::detail::tls_transport_model<Transport>>(std::move(transport)),
                 configuration, pre_read) {}
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
    task<std::size_t> read_some(std::span<char> buffer);
    task<void> write_all(std::string_view bytes);
    task<std::size_t> read_some(std::span<std::byte> buffer) {
        return read_some(std::span<char>(reinterpret_cast<char*>(buffer.data()), buffer.size()));
    }
    task<void> write_all(std::span<const std::byte> bytes) {
        return write_all(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }
    // Exchange close_notify cooperatively, closing even on cancellation/error or
    // timeout. Expiring this bound closes normally, without promising peer receipt.
    task<void> shutdown(std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
    task<void> close_gracefully(std::chrono::milliseconds timeout = std::chrono::milliseconds(100)) {
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
    stream(std::unique_ptr<pace::detail::tls_transport> transport,
           const tls::context& configuration, std::string_view pre_read);
    friend class pace::detail::tls_driver;
    task<void> connect_impl(tcp::endpoint endpoint, std::string peer_name, std::optional<std::string> server_name);
    task<void> handshake_impl(std::string peer_name, std::optional<std::string> server_name);
    task<void> perform_handshake();
};

// Complete a handshake using the supplied verification policy. Custom
// configuration is borrowed through task completion; streams retain its state.
inline task<stream> connect(io_context& io, const context& configuration,
                             tcp::endpoint peer, std::string expected_name) {
    stream result(io, configuration);
    co_await result.connect(std::move(peer), std::move(expected_name));
    co_return result;
}
inline task<stream> connect(const context& configuration, tcp::endpoint peer, std::string expected_name) {
    co_return co_await connect(this_context(), configuration, std::move(peer), std::move(expected_name));
}
// System trust for a client that needs no custom CA or ALPN configuration.
inline task<stream> connect(io_context& io, tcp::endpoint peer, std::string expected_name) {
    context configuration;
    co_return co_await connect(io, configuration, std::move(peer), std::move(expected_name));
}
inline task<stream> connect(tcp::endpoint peer, std::string expected_name) {
    co_return co_await connect(this_context(), std::move(peer), std::move(expected_name));
}
inline task<stream> accept(tcp::stream transport, const context& configuration) {
    stream result(std::move(transport), configuration);
    co_await result.handshake();
    co_return result;
}
} // namespace pace::tls
#endif // PACE_HAS_TLS
