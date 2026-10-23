#include <pace/tls.hpp>
#include <pace/detail/protocol_driver.hpp>

#ifdef PACE_HAS_TLS
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <map>
#include <mutex>
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

std::string dns_name(std::string_view value) {
    if (value.empty() || value.size() > 253)
        throw std::invalid_argument("TLS SNI requires an ASCII DNS name");
    std::string result(value);
    std::size_t label = 0;
    char previous = '\0';
    for (auto& c : result) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        if (c == '.') {
            if (label == 0 || label > 63 || previous == '-')
                throw std::invalid_argument("invalid TLS SNI DNS label");
            label = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') ||
                (label == 0 && c == '-'))
                throw std::invalid_argument("invalid TLS SNI DNS name");
            ++label;
        }
        previous = c;
    }
    unsigned char address[16];
    if (label == 0 || label > 63 || result.back() == '-' ||
        ::inet_pton(AF_INET, result.c_str(), address) == 1)
        throw std::invalid_argument("TLS SNI requires a DNS name, not an IP address");
    return result;
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
    std::mutex mutex;
    bool frozen = false;
    std::vector<unsigned char> alpn;
    std::map<std::string, std::shared_ptr<implementation>, std::less<>> names;
    sni_policy missing = sni_policy::use_default;
    sni_policy unrecognized = sni_policy::use_default;
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> handle{nullptr, SSL_CTX_free};
    explicit implementation(role value, trust_source trust) : mode(value) {
        if (mode != role::client && mode != role::server)
            throw std::invalid_argument("invalid TLS context role");
        if (trust != trust_source::system && trust != trust_source::application)
            throw std::invalid_argument("invalid TLS trust source");
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
            if (trust == trust_source::system && SSL_CTX_set_default_verify_paths(handle.get()) != 1)
                tls_error("load OpenSSL default trust paths");
        }
    }
    void check_mutable() const {
        if (frozen) throw std::logic_error("TLS configuration is frozen");
    }
    void freeze() {
        std::lock_guard lock(mutex);
        frozen = true;
    }
    void enable_sni() {
        // The servername callback runs after session resumption. Disable both
        // TLS 1.2 and TLS 1.3 resumption on routing contexts so a ticket cannot
        // bypass this connection's independent hostname/certificate selection.
        SSL_CTX_set_session_cache_mode(handle.get(), SSL_SESS_CACHE_OFF);
        SSL_CTX_set_options(handle.get(), SSL_OP_NO_TICKET);
        if (SSL_CTX_set_num_tickets(handle.get(), 0) != 1)
            tls_error("disable TLS SNI session tickets");
        SSL_CTX_set_tlsext_servername_arg(handle.get(), this);
        // OpenSSL casts this callback inside a macro. Keep the lambda outside
        // that expansion so MSVC parses its parameter list and body correctly.
        const auto callback =
            +[](SSL* ssl, int* alert, void* argument) noexcept -> int {
                const auto& config = *static_cast<implementation*>(argument);
                try {
                    const char* requested = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
                    if (requested) {
                        const auto name = dns_name(requested);
                        const auto found = config.names.find(name);
                        if (found != config.names.end()) {
                            if (!SSL_set_SSL_CTX(ssl, found->second->handle.get())) {
                                *alert = SSL_AD_INTERNAL_ERROR;
                                return SSL_TLSEXT_ERR_ALERT_FATAL;
                            }
                            return SSL_TLSEXT_ERR_OK;
                        }
                    }
                    if ((requested ? config.unrecognized : config.missing) == sni_policy::reject) {
                        *alert = SSL_AD_UNRECOGNIZED_NAME;
                        return SSL_TLSEXT_ERR_ALERT_FATAL;
                    }
                    return SSL_TLSEXT_ERR_NOACK;
                } catch (...) {
                    // No C++ exception may cross OpenSSL's callback boundary.
                    *alert = SSL_AD_UNRECOGNIZED_NAME;
                    return SSL_TLSEXT_ERR_ALERT_FATAL;
                }
            };
        SSL_CTX_set_tlsext_servername_callback(handle.get(), callback);
    }
};

context::context(role mode, trust_source trust) : impl_(std::make_shared<implementation>(mode, trust)) {}
context::~context() = default;
context::context(context&&) noexcept = default;
context& context::operator=(context&&) noexcept = default;
void context::load_verify_file(const std::string& file) {
    check_path(file);
    if (!impl_) throw std::logic_error("moved-from TLS context");
    std::lock_guard lock(impl_->mutex);
    impl_->check_mutable();
    ERR_clear_error();
    if (SSL_CTX_load_verify_locations(impl_->handle.get(), file.c_str(), nullptr) != 1)
        tls_error("load TLS trust file");
}
void context::load_verify_pem(std::string_view certificates) {
    if (!impl_) throw std::logic_error("moved-from TLS context");
    std::lock_guard lock(impl_->mutex);
    impl_->check_mutable();
    std::vector<std::unique_ptr<X509, decltype(&X509_free)>> parsed;
    constexpr std::string_view begin = "-----BEGIN CERTIFICATE-----";
    constexpr std::string_view end = "-----END CERTIFICATE-----";
    while (!certificates.empty()) {
        const auto first = certificates.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) break;
        certificates.remove_prefix(first);
        const auto last = certificates.find(end);
        if (!certificates.starts_with(begin) || last == std::string_view::npos ||
            last + end.size() > INT_MAX)
            throw std::invalid_argument("TLS trust PEM must contain only complete CERTIFICATE blocks");
        const auto block = certificates.substr(0, last + end.size());
        ERR_clear_error();
        std::unique_ptr<BIO, decltype(&BIO_free)> input(
            BIO_new_mem_buf(block.data(), static_cast<int>(block.size())), BIO_free);
        if (!input) tls_error("create TLS trust PEM input");
        std::unique_ptr<X509, decltype(&X509_free)> cert(
            PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr), X509_free);
        if (!cert) tls_error("parse TLS trust PEM");
        parsed.push_back(std::move(cert));
        certificates.remove_prefix(block.size());
    }
    if (parsed.empty()) throw std::invalid_argument("TLS trust PEM contains no certificates");
    // Parse the entire bundle before changing the store. The store takes its own
    // X509 references, with no application memory or mutable native handle kept.
    auto* store = SSL_CTX_get_cert_store(impl_->handle.get());
    for (const auto& cert : parsed) {
        ERR_clear_error();
        if (X509_STORE_add_cert(store, cert.get()) != 1) {
            const auto error = ERR_peek_last_error();
            if (ERR_GET_LIB(error) != ERR_LIB_X509 ||
                ERR_GET_REASON(error) != X509_R_CERT_ALREADY_IN_HASH_TABLE)
                tls_error("add TLS trust certificate");
            ERR_clear_error();
        }
    }
}
void context::use_certificate_chain(const std::string& certificate, const std::string& key) {
    check_path(certificate); check_path(key);
    if (!impl_) throw std::logic_error("moved-from TLS context");
    std::lock_guard lock(impl_->mutex);
    impl_->check_mutable();
    ERR_clear_error();
    // Stage the entire pair before touching the live configuration. In
    // particular, a failed key load must not replace a previously valid leaf.
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> staged(SSL_CTX_new(TLS_method()), SSL_CTX_free);
    if (!staged) tls_error("create TLS certificate staging context");
    SSL_CTX_set_default_passwd_cb(staged.get(), [](char*, int, int, void*) { return 0; });
    if (SSL_CTX_use_certificate_chain_file(staged.get(), certificate.c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(staged.get(), key.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(staged.get()) != 1)
        tls_error("load TLS certificate/private key");
    STACK_OF(X509)* chain = nullptr;
    SSL_CTX_get0_chain_certs(staged.get(), &chain);
    if (SSL_CTX_use_cert_and_key(impl_->handle.get(), SSL_CTX_get0_certificate(staged.get()),
                                SSL_CTX_get0_privatekey(staged.get()), chain, 1) != 1)
        tls_error("install TLS certificate/private key");
}
void context::set_alpn_protocols(const std::vector<std::string>& protocols) {
    if (!impl_) throw std::logic_error("moved-from TLS context");
    std::lock_guard lock(impl_->mutex);
    impl_->check_mutable();
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
void context::add_server_name(std::string name, const context& configuration) {
    if (!impl_ || !configuration.impl_) throw std::logic_error("moved-from TLS context");
    if (impl_ == configuration.impl_) throw std::invalid_argument("TLS SNI target cannot be itself");
    name = dns_name(name);
    std::scoped_lock lock(impl_->mutex, configuration.impl_->mutex);
    impl_->check_mutable();
    if (impl_->mode != role::server || configuration.impl_->mode != role::server)
        throw std::invalid_argument("TLS SNI requires server contexts");
    if (!configuration.impl_->names.empty())
        throw std::invalid_argument("TLS SNI target cannot contain another routing table");
    if (impl_->names.contains(name)) throw std::invalid_argument("duplicate TLS SNI name");
    impl_->enable_sni();
    impl_->names.emplace(std::move(name), configuration.impl_);
    configuration.impl_->frozen = true;
}
void context::set_server_name_policy(sni_policy missing, sni_policy unrecognized) {
    if (!impl_) throw std::logic_error("moved-from TLS context");
    if ((missing != sni_policy::use_default && missing != sni_policy::reject) ||
        (unrecognized != sni_policy::use_default && unrecognized != sni_policy::reject))
        throw std::invalid_argument("invalid TLS SNI policy");
    std::lock_guard lock(impl_->mutex);
    impl_->check_mutable();
    if (impl_->mode != role::server) throw std::invalid_argument("TLS SNI requires a server context");
    impl_->enable_sni();
    impl_->missing = missing;
    impl_->unrecognized = unrecognized;
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
        configuration->freeze();
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
    bool timed_out = false;
    io_context::timer_handle deadline;
    try {
        p.check(true);
        if (timeout > std::chrono::milliseconds::zero()) {
            deadline = p.transport.context().call_after(timeout, [&] {
                timed_out = true;
                p.transport.close();
            });
        }
        for (;;) {
            // Readiness may already have queued this continuation before the
            // deadline closed the fd; in that case the waiter has no cancel code.
            if (timed_out && !p.transport.context().stopped()) break;
            p.check(true);
            ERR_clear_error();
            const int result = SSL_shutdown(p.ssl.get());
            if (result == 1) break;
            const int error = result < 0 ? SSL_get_error(p.ssl.get(), result) : SSL_ERROR_WANT_READ;
            if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
                tls_error("TLS shutdown failed", p.ssl.get());
            if (timeout <= std::chrono::milliseconds::zero()) break;
            co_await p.wait(error);
        }
    } catch (const std::system_error& error) {
        close();
        if (timed_out && error.code() == std::errc::operation_canceled &&
            !p.transport.context().stopped()) co_return;
        throw;
    } catch (...) { close(); throw; }
    close();
}

} // namespace pace::tls

namespace pace::detail {
tls_driver::tls_driver(tls::stream& stream) : stream_(stream) {
    if (!stream_.impl_) throw std::logic_error("moved-from TLS stream");
    auto& p = *stream_.impl_;
    if (p.active) throw std::logic_error("another TLS operation is pending");
    p.check(true);
    p.active = true;
}
tls_driver::~tls_driver() { stream_.impl_->active = false; }
native_socket_type tls_driver::native_handle() const noexcept { return stream_.impl_->transport.native_handle(); }
bool tls_driver::write_wait() const noexcept { return read_wants_write_ || (write_retry_ && write_wants_write_); }
std::optional<std::size_t> tls_driver::read(std::span<char> bytes) {
    auto& p = *stream_.impl_;
    p.check(true);
    if (!read_allowed()) return std::nullopt;
    if (p.eof || bytes.empty()) return 0;
    std::size_t count = 0;
    ERR_clear_error();
    const int result = SSL_read_ex(p.ssl.get(), bytes.data(), bytes.size(), &count);
    if (result == 1) { read_wants_write_ = false; return count; }
    const int error = SSL_get_error(p.ssl.get(), result);
    if (error == SSL_ERROR_ZERO_RETURN) { p.eof = true; read_wants_write_ = false; return 0; }
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
        read_wants_write_ = error == SSL_ERROR_WANT_WRITE;
        return std::nullopt;
    }
    tls::tls_error("TLS protocol driver read failed", p.ssl.get());
}
std::size_t tls_driver::write(std::string_view bytes) {
    auto& p = *stream_.impl_;
    p.check(true);
    if (!write_allowed() || bytes.empty()) return 0;
    // OpenSSL may have consumed part of the record even when it reports WANT.
    // Neither its input pointer nor length/data may change until retry succeeds.
    if (write_retry_ && (write_data_ != bytes.data() || write_size_ != bytes.size()))
        throw std::logic_error("TLS write retry changed its buffer");
    std::size_t count = 0;
    ERR_clear_error();
    const int result = SSL_write_ex(p.ssl.get(), bytes.data(), bytes.size(), &count);
    if (result == 1) { write_retry_ = false; write_wants_write_ = false; return count; }
    const int error = SSL_get_error(p.ssl.get(), result);
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
        write_retry_ = true;
        write_wants_write_ = error == SSL_ERROR_WANT_WRITE;
        write_data_ = bytes.data(); write_size_ = bytes.size();
        return 0;
    }
    tls::tls_error("TLS protocol driver write failed", p.ssl.get());
}
} // namespace pace::detail
#endif // PACE_HAS_TLS
