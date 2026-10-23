#pragma once

#include <pace/tcp.hpp>

#ifdef PACE_HAS_TLS

#include <memory>
#include <string>
#include <vector>

namespace pace::tls {

enum class role { client, server };
enum class trust_source { system, application };
enum class sni_policy { use_default, reject };

// Configuration freezes on first stream construction or registration as an SNI
// target; subsequent setters throw logic_error. Streams retain shared ownership
// of their configuration and every SNI target. Clients always verify the peer.
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

// Move-only TLS over an owned TCP stream. Exactly one asynchronous operation may
// be active at a time (including reads vs writes). Move only while idle; preserve
// this object's address and borrowed buffers until each operation finishes.
class stream {
public:
    stream(io_context& io, const tls::context& configuration);
    stream(tcp::stream transport, const tls::context& configuration);
    ~stream();
    stream(stream&&) noexcept;
    stream& operator=(stream&&) noexcept;
    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;

    // Client name is mandatory and checked against the certificate, independently
    // of the numeric TCP endpoint. DNS names also configure SNI. No insecure mode.
    task<void> connect(tcp::endpoint endpoint, std::string peer_name);
    task<void> handshake(std::string peer_name = {});
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
    task<void> perform_handshake(std::string peer_name);
};

} // namespace pace::tls
#endif // PACE_HAS_TLS
