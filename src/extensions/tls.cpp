#include <pace/tls.hpp>

#ifdef PACE_HAS_TLS
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <stdexcept>
#include <utility>
#if !defined(_WIN32)
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

namespace pace::tls {
namespace {
[[noreturn]] void tls_error(const char* operation, SSL* ssl = nullptr) {
    std::string message(operation);
    if (ssl && SSL_get_verify_result(ssl) != X509_V_OK) {
        message += ": ";
        message += X509_verify_cert_error_string(SSL_get_verify_result(ssl));
    }
    for (unsigned long code; (code = ERR_get_error()) != 0;) {
        char text[256]{};
        ERR_error_string_n(code, text, sizeof(text));
        message += ": ";
        message += text;
    }
    throw std::system_error(std::make_error_code(std::errc::protocol_error), message);
}

void check_path(const std::string& path) {
    if (path.empty() || path.find('\0') != std::string::npos)
        throw std::invalid_argument("TLS PEM path must be nonempty without NUL bytes");
}

bool socket_interrupted() noexcept {
#ifdef _WIN32
    return ::WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}
bool socket_would_block() noexcept {
#ifdef _WIN32
    return ::WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

// OpenSSL's stock socket BIO takes an int descriptor. A custom BIO preserves
// the full Winsock handle and uses SIGPIPE-safe sends on POSIX.
int bio_read(BIO* bio, char* bytes, int size) {
    BIO_clear_retry_flags(bio);
    if (!bytes || size <= 0) return 0;
    const auto fd = static_cast<tcp::stream*>(BIO_get_data(bio))->native_handle();
    int result;
    do { result = static_cast<int>(::recv(fd, bytes, size, 0)); }
    while (result < 0 && socket_interrupted());
    if (result < 0 && socket_would_block()) BIO_set_retry_read(bio);
    return result;
}
int bio_write(BIO* bio, const char* bytes, int size) {
    BIO_clear_retry_flags(bio);
    if (!bytes || size <= 0) return 0;
    const auto fd = static_cast<tcp::stream*>(BIO_get_data(bio))->native_handle();
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    int result;
    do { result = static_cast<int>(::send(fd, bytes, size, flags)); }
    while (result < 0 && socket_interrupted());
    if (result < 0 && socket_would_block()) BIO_set_retry_write(bio);
    return result;
}
BIO_METHOD* socket_method() {
    static const auto method = [] {
        std::unique_ptr<BIO_METHOD, decltype(&BIO_meth_free)> result(
            BIO_meth_new(BIO_TYPE_SOURCE_SINK | BIO_get_new_index(), "PACE nonblocking socket"), BIO_meth_free);
        if (!result || !BIO_meth_set_read(result.get(), bio_read) ||
            !BIO_meth_set_write(result.get(), bio_write) ||
            !BIO_meth_set_create(result.get(), [](BIO* bio) { BIO_set_init(bio, 1); return 1; }) ||
            !BIO_meth_set_destroy(result.get(), [](BIO*) { return 1; }) ||
            !BIO_meth_set_ctrl(result.get(), [](BIO*, int command, long, void*) -> long {
                return command == BIO_CTRL_FLUSH ? 1 : 0;
            })) tls_error("create socket BIO method");
        return result;
    }();
    return method.get();
}
} // namespace

struct context::implementation {
    role mode;
    std::vector<unsigned char> alpn;
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> handle{nullptr, SSL_CTX_free};
    explicit implementation(role value) : mode(value) {
        if (mode != role::client && mode != role::server)
            throw std::invalid_argument("invalid TLS context role");
        ERR_clear_error();
        handle.reset(SSL_CTX_new(mode == role::client ? TLS_client_method() : TLS_server_method()));
        if (!handle || SSL_CTX_set_min_proto_version(handle.get(), TLS1_2_VERSION) != 1)
            tls_error("create TLS 1.2+ context");
        SSL_CTX_set_options(handle.get(), SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
        SSL_CTX_set_mode(handle.get(), SSL_MODE_ENABLE_PARTIAL_WRITE);
        // Loading an encrypted key must fail instead of prompting on stdin.
        SSL_CTX_set_default_passwd_cb(handle.get(), [](char*, int, int, void*) { return 0; });
        if (mode == role::client) {
            SSL_CTX_set_verify(handle.get(), SSL_VERIFY_PEER, nullptr);
            if (SSL_CTX_set_default_verify_paths(handle.get()) != 1)
                tls_error("load OpenSSL default trust paths");
        }
    }
};

context::context(role mode) : impl_(std::make_shared<implementation>(mode)) {}
context::~context() = default;
context::context(context&&) noexcept = default;
context& context::operator=(context&&) noexcept = default;
void context::load_verify_file(const std::string& file) {
    check_path(file);
    if (!impl_) throw std::logic_error("moved-from TLS context");
    ERR_clear_error();
    if (SSL_CTX_load_verify_locations(impl_->handle.get(), file.c_str(), nullptr) != 1)
        tls_error("load TLS trust file");
}
void context::use_certificate_chain(const std::string& certificate, const std::string& key) {
    check_path(certificate); check_path(key);
    if (!impl_) throw std::logic_error("moved-from TLS context");
    ERR_clear_error();
    if (SSL_CTX_use_certificate_chain_file(impl_->handle.get(), certificate.c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(impl_->handle.get(), key.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(impl_->handle.get()) != 1)
        tls_error("load TLS certificate/private key");
}
void context::set_alpn_protocols(const std::vector<std::string>& protocols) {
    if (!impl_) throw std::logic_error("moved-from TLS context");
    std::vector<unsigned char> encoded;
    for (const auto& protocol : protocols) {
        if (protocol.empty() || protocol.size() > 255 || encoded.size() + protocol.size() + 1 > 65535)
            throw std::invalid_argument("invalid TLS ALPN protocol list");
        encoded.push_back(static_cast<unsigned char>(protocol.size()));
        encoded.insert(encoded.end(), protocol.begin(), protocol.end());
    }
    ERR_clear_error();
    if (impl_->mode == role::client) {
        if (SSL_CTX_set_alpn_protos(impl_->handle.get(), encoded.data(), static_cast<unsigned>(encoded.size())) != 0)
            tls_error("configure TLS client ALPN");
    } else {
        SSL_CTX_set_alpn_select_cb(impl_->handle.get(), encoded.empty() ? nullptr :
            +[](SSL*, const unsigned char** output, unsigned char* output_length,
                const unsigned char* input, unsigned input_length, void* argument) -> int {
                const auto& supported = static_cast<implementation*>(argument)->alpn;
                unsigned char* selected = nullptr;
                const int result = SSL_select_next_proto(&selected, output_length,
                    supported.data(), static_cast<unsigned>(supported.size()), input, input_length);
                if (result != OPENSSL_NPN_NEGOTIATED) return SSL_TLSEXT_ERR_ALERT_FATAL;
                *output = selected;
                return SSL_TLSEXT_ERR_OK;
            }, impl_.get());
    }
    impl_->alpn = std::move(encoded);
}

struct stream::implementation {
    tcp::stream transport;
    std::shared_ptr<tls::context::implementation> configuration;
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl{nullptr, SSL_free};
    bool active = false;
    bool handshaken = false;
    bool eof = false;

    implementation(tcp::stream socket, std::shared_ptr<tls::context::implementation> config)
        : transport(std::move(socket)), configuration(std::move(config)) {
        if (!configuration) throw std::logic_error("moved-from TLS context");
        ERR_clear_error();
        ssl.reset(SSL_new(configuration->handle.get()));
        if (!ssl) tls_error("create TLS session");
        auto* bio = BIO_new(socket_method());
        if (!bio) tls_error("create TLS socket BIO");
        BIO_set_data(bio, &transport);
        SSL_set_bio(ssl.get(), bio, bio);
        if (configuration->mode == role::client) SSL_set_connect_state(ssl.get());
        else SSL_set_accept_state(ssl.get());
    }
    void check(bool require_handshake = false) const {
        if (transport.context().stopped())
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        if (!transport.is_open())
            throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor));
        if (io_context::current() != &transport.context())
            throw std::logic_error("TLS operation must execute on its owning io_context");
        if (require_handshake && !handshaken)
            throw std::logic_error("TLS handshake has not completed");
    }
    struct operation {
        implementation& state;
        explicit operation(implementation& value) : state(value) {
            if (state.active) throw std::logic_error("TLS stream allows only one active operation");
            state.active = true;
        }
        ~operation() { state.active = false; }
    };
    task<void> wait(int error) {
        if (error == SSL_ERROR_WANT_READ)
            co_await transport.context().wait_readable(transport.native_handle());
        else if (error == SSL_ERROR_WANT_WRITE)
            co_await transport.context().wait_writable(transport.native_handle());
        else tls_error("TLS operation failed (possibly unclean EOF)", ssl.get());
    }
};

stream::stream(io_context& io, const tls::context& config) : stream(tcp::stream(io), config) {}
stream::stream(tcp::stream transport, const tls::context& config)
    : impl_(std::make_unique<implementation>(std::move(transport), config.impl_)) {}
stream::~stream() = default;
stream::stream(stream&&) noexcept = default;
stream& stream::operator=(stream&&) noexcept = default;
io_context& stream::context() const noexcept { return impl_->transport.context(); }
bool stream::is_open() const noexcept { return impl_ && impl_->transport.is_open(); }
void stream::close() noexcept { if (impl_) impl_->transport.close(); }
void stream::set_no_delay(bool enabled) {
    if (!impl_) throw std::logic_error("moved-from TLS stream");
    impl_->transport.set_no_delay(enabled);
}
std::string stream::selected_protocol() const {
    if (!impl_) throw std::logic_error("moved-from TLS stream");
    const unsigned char* protocol = nullptr;
    unsigned length = 0;
    SSL_get0_alpn_selected(impl_->ssl.get(), &protocol, &length);
    return length ? std::string(reinterpret_cast<const char*>(protocol), length) : std::string{};
}

task<void> stream::perform_handshake(std::string peer_name) {
    auto& p = *impl_;
    p.check();
    if (p.handshaken) throw std::logic_error("TLS handshake already completed");
    if (p.configuration->mode == role::client) {
        if (peer_name.empty() || peer_name.find('\0') != std::string::npos)
            throw std::invalid_argument("TLS client requires a nonempty peer DNS name or IP address");
        auto* parameters = SSL_get0_param(p.ssl.get());
        X509_VERIFY_PARAM_set_hostflags(parameters, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        unsigned char address[16];
        const bool ip = ::inet_pton(AF_INET, peer_name.c_str(), address) == 1 ||
                        ::inet_pton(AF_INET6, peer_name.c_str(), address) == 1;
        ERR_clear_error();
        if (ip) {
            if (X509_VERIFY_PARAM_set1_ip_asc(parameters, peer_name.c_str()) != 1)
                tls_error("set TLS peer IP verification");
        } else {
            if (SSL_set1_host(p.ssl.get(), peer_name.c_str()) != 1 ||
                SSL_set_tlsext_host_name(p.ssl.get(), peer_name.c_str()) != 1)
                tls_error("set TLS peer name/SNI");
        }
    } else if (!peer_name.empty()) throw std::invalid_argument("TLS server handshake does not take a peer name");
    for (;;) {
        p.check();
        ERR_clear_error();
        const int result = SSL_do_handshake(p.ssl.get());
        if (result == 1) {
            if (p.configuration->mode == role::client && SSL_get_verify_result(p.ssl.get()) != X509_V_OK)
                tls_error("TLS peer verification failed", p.ssl.get());
            p.handshaken = true;
            co_return;
        }
        const int error = SSL_get_error(p.ssl.get(), result);
        co_await p.wait(error);
    }
}
task<void> stream::connect(tcp::endpoint endpoint, std::string peer_name) {
    if (!impl_) throw std::logic_error("moved-from TLS stream");
    implementation::operation operation(*impl_);
    try {
        impl_->check();
        if (impl_->configuration->mode != role::client)
            throw std::logic_error("TLS connect requires a client context");
        if (peer_name.empty() || peer_name.find('\0') != std::string::npos)
            throw std::invalid_argument("TLS connect requires a peer name");
        co_await impl_->transport.connect(std::move(endpoint));
        co_await perform_handshake(std::move(peer_name));
    } catch (...) { close(); throw; }
}
task<void> stream::handshake(std::string peer_name) {
    if (!impl_) throw std::logic_error("moved-from TLS stream");
    implementation::operation operation(*impl_);
    try { co_await perform_handshake(std::move(peer_name)); }
    catch (...) { close(); throw; }
}
task<std::size_t> stream::read_some(std::span<char> buffer) {
    if (!impl_) throw std::logic_error("moved-from TLS stream");
    auto& p = *impl_;
    implementation::operation operation(p);
    try {
        for (;;) {
            p.check(true);
            if (buffer.empty() || p.eof) co_return 0;
            std::size_t count = 0;
            ERR_clear_error();
            const int result = SSL_read_ex(p.ssl.get(), buffer.data(), buffer.size(), &count);
            if (result == 1) co_return count;
            const int error = SSL_get_error(p.ssl.get(), result);
            if (error == SSL_ERROR_ZERO_RETURN) { p.eof = true; co_return 0; }
            co_await p.wait(error);
        }
    } catch (...) { close(); throw; }
}
task<void> stream::write_all(std::string_view bytes) {
    if (!impl_) throw std::logic_error("moved-from TLS stream");
    auto& p = *impl_;
    implementation::operation operation(p);
    try {
        for (;;) {
            p.check(true);
            if (bytes.empty()) co_return;
            std::size_t count = 0;
            ERR_clear_error();
            const int result = SSL_write_ex(p.ssl.get(), bytes.data(), bytes.size(), &count);
            if (result == 1) { bytes.remove_prefix(count); continue; }
            const int error = SSL_get_error(p.ssl.get(), result);
            co_await p.wait(error);
        }
    } catch (...) { close(); throw; }
}
task<void> stream::shutdown(std::chrono::milliseconds timeout) {
    if (!impl_) throw std::logic_error("moved-from TLS stream");
    auto& p = *impl_;
    implementation::operation operation(p);
    try {
        p.check(true);
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            p.check(true);
            ERR_clear_error();
            const int result = SSL_shutdown(p.ssl.get());
            if (result == 1) break;
            const int error = result < 0 ? SSL_get_error(p.ssl.get(), result) : SSL_ERROR_WANT_READ;
            if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
                tls_error("TLS shutdown failed", p.ssl.get());
            if (std::chrono::duration<long double>(std::chrono::steady_clock::now() - start) >=
                std::chrono::duration<long double>(timeout)) break;
            // Timer polling makes the bound independent of a peer's close_notify.
            // No detached timeout coroutine can outlive the stream.
            co_await p.transport.context().sleep_for(std::chrono::milliseconds(1));
        }
    } catch (...) { close(); throw; }
    close();
}

} // namespace pace::tls
#endif // PACE_HAS_TLS
