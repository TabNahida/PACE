#include <pace/quic.hpp>
#ifdef PACE_HAS_QUIC
#include <pace/detail/socket_address.hpp>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#if OPENSSL_VERSION_NUMBER < 0x30500000L || defined(OPENSSL_NO_QUIC)
#error PACE_QUIC_requires_OpenSSL_3_5_or_newer_with_QUIC
#endif
#if defined(_WIN32)
#error PACE_QUIC_requires_POSIX_sockets_Windows_transport_is_not_implemented
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#if defined(__linux__)
#include <sys/timerfd.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <array>
#include <list>
#include <stdexcept>
#include <utility>

namespace pace::quic {
namespace {
using namespace std::chrono_literals;
[[noreturn]] void fail(const char* where) {
    std::string text(where);
    while (auto code = ERR_get_error()) { char buffer[256]; ERR_error_string_n(code, buffer, sizeof(buffer)); text += ": "; text += buffer; }
    throw std::system_error(std::make_error_code(std::errc::protocol_error), text);
}
void required(int result, const char* where) { if (result != 1) fail(where); }
bool retry(SSL* ssl, int result) {
    const auto error = SSL_get_error(ssl, result);
    return error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE;
}
void affinity(io_context& io) {
    if (io.stopped()) throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    if (io_context::current() != &io) throw std::logic_error("QUIC operation must run on its owning context");
}
struct guard {
    bool& flag;
    explicit guard(bool& value) : flag(value) { if (flag) throw std::logic_error("overlapping QUIC operation"); flag = true; }
    ~guard() { flag = false; }
};
void set_bio(SSL* ssl, udp::socket& socket) {
    BIO* bio = BIO_new_dgram(socket.native_handle(), BIO_NOCLOSE);
    if (!bio) fail("QUIC datagram BIO");
    SSL_set_bio(ssl, bio, bio);
    required(SSL_set_blocking_mode(ssl, 0), "QUIC nonblocking mode");
}
} // namespace
namespace detail {
struct configuration {
    SSL_CTX* ssl = nullptr;
    role mode;
    std::vector<unsigned char> alpn;
    explicit configuration(role value) : mode(value) {
        if (mode != role::client && mode != role::server) throw std::invalid_argument("invalid QUIC context role");
        ssl = SSL_CTX_new(value == role::client ? OSSL_QUIC_client_method() : OSSL_QUIC_server_method());
        if (!ssl) fail("QUIC context");
        SSL_CTX_set_default_passwd_cb(ssl, [](char*, int, int, void*) { return 0; });
        if (mode == role::client) { SSL_CTX_set_verify(ssl, SSL_VERIFY_PEER, nullptr); SSL_CTX_set_default_verify_paths(ssl); }
        else SSL_CTX_set_alpn_select_cb(ssl, [](SSL*, const unsigned char** out, unsigned char* size, const unsigned char* in, unsigned int length, void* arg) {
            const auto& protocols = static_cast<configuration*>(arg)->alpn;
            unsigned char* selected = nullptr;
            if (SSL_select_next_proto(&selected, size, protocols.data(), static_cast<unsigned>(protocols.size()), in, length) != OPENSSL_NPN_NEGOTIATED)
                return SSL_TLSEXT_ERR_ALERT_FATAL;
            *out = selected; return SSL_TLSEXT_ERR_OK;
        }, this);
    }
    ~configuration() { SSL_CTX_free(ssl); }
};
// One readiness reader per UDP socket. OpenSSL demultiplexes every connection
// on a server listener, so accepted connections share this event engine.
struct engine : std::enable_shared_from_this<engine> {
    using clock = std::chrono::steady_clock;
    struct event {
        engine& owner;
        clock::time_point deadline = clock::time_point::max();
        std::coroutine_handle<> continuation{};
        std::list<event*>* queue = nullptr;
        std::list<event*>::iterator position{};
        ~event() { if (queue) queue->erase(position); }
        bool await_ready() { owner.check(); return false; }
        void await_suspend(std::coroutine_handle<> handle) {
            continuation = handle;
            position = owner.waiters.insert(owner.waiters.end(), this);
            queue = &owner.waiters;
            owner.refresh();
        }
        void await_resume() { owner.check(); }
    };
    io_context& io;
    udp::socket socket;
    std::shared_ptr<configuration> config;
    SSL* ssl = nullptr;
    bool open = true, started = false, accepting = false, write_waiting = false, tick_queued = false;
    std::exception_ptr failure;
    std::list<event*> waiters;
#if defined(__linux__)
    int timer = -1;
#endif
    engine(io_context& owner, udp::endpoint local, std::shared_ptr<configuration> settings)
        : io(owner), socket(owner, std::move(local)), config(std::move(settings)) {
#if defined(__linux__)
        timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (timer < 0) throw std::system_error(errno, std::generic_category(), "QUIC timerfd");
#endif
    }
    ~engine() {
        close();
        SSL_free(ssl);
    }
    void check() {
        affinity(io);
        if (failure) std::rethrow_exception(failure);
        if (!open) throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    }
    // Resume a detached list. A resumed operation may register a new wait or
    // destroy another waiting operation; its destructor unlinks from this list.
    void notify() noexcept {
        std::list<event*> ready;
        ready.splice(ready.end(), waiters);
        for (auto* waiter : ready) waiter->queue = &ready;
        while (!ready.empty()) {
            auto* waiter = ready.front(); ready.pop_front(); waiter->queue = nullptr;
            waiter->continuation.resume();
        }
    }
    void close() noexcept {
        if (!open) return;
        open = false;
        socket.close();
#if defined(__linux__)
        if (timer >= 0) { io.cancel(timer); ::close(timer); timer = -1; }
#endif
        notify();
    }
    clock::duration timeout() {
        timeval value{}; int infinite = 0;
        required(SSL_get_event_timeout(ssl, &value, &infinite), "QUIC next event timeout");
        auto delay = infinite ? clock::duration::max() :
            std::chrono::duration_cast<clock::duration>(std::chrono::seconds(value.tv_sec) + std::chrono::microseconds(value.tv_usec));
        const auto now = clock::now();
        for (auto* waiter : waiters) {
            if (waiter->deadline != clock::time_point::max()) delay = std::min(delay, waiter->deadline - now);
        }
        return delay;
    }
    void refresh() {
        if (!open || !started || io.stopped()) return;
#if defined(__linux__)
        const auto delay = timeout();
        itimerspec value{};
        if (delay != clock::duration::max()) {
            // timerfd uses zero to disarm; an already due event needs 1 ns.
            const auto ns = std::max(std::chrono::duration_cast<std::chrono::nanoseconds>(delay).count(), INT64_C(1));
            value.it_value.tv_sec = ns / 1000000000;
            value.it_value.tv_nsec = ns % 1000000000;
        }
        if (timerfd_settime(timer, 0, &value, nullptr) < 0)
            throw std::system_error(errno, std::generic_category(), "QUIC timer arm");
#endif
        if (!write_waiting && SSL_net_write_desired(ssl)) {
            write_waiting = true;
            io.spawn(write_pump(weak_from_this()));
        }
    }
    void schedule_tick() {
        if (!open || io.stopped() || tick_queued) return;
        tick_queued = true;
        io.post([weak = weak_from_this()] {
            if (auto self = weak.lock(); self && self->open) {
                self->tick_queued = false;
                try { self->check(); self->tick(); }
                catch (...) { self->failure = std::current_exception(); self->close(); }
            }
        });
    }
    void tick() {
        ERR_clear_error(); required(SSL_handle_events(ssl), "QUIC event handling");
        notify();
        refresh();
    }
    static task<void> read_pump(std::weak_ptr<engine> weak) {
        for (;;) {
            io_context* io = nullptr; native_socket_type fd = invalid_socket;
            { auto self = weak.lock(); if (!self || !self->open) co_return;
              io = &self->io; fd = self->socket.native_handle(); }
            try {
                co_await io->wait_readable(fd);
                auto self = weak.lock(); if (!self || !self->open) co_return;
                self->tick();
            } catch (...) {
                if (auto self = weak.lock(); self && self->open) {
                    self->failure = std::current_exception(); self->close();
                }
                co_return;
            }
        }
    }
    static task<void> write_pump(std::weak_ptr<engine> weak) {
        for (;;) {
            io_context* io = nullptr; native_socket_type fd = invalid_socket;
            { auto self = weak.lock(); if (!self || !self->open) co_return;
              io = &self->io; fd = self->socket.native_handle(); }
            try {
                co_await io->wait_writable(fd);
                auto self = weak.lock(); if (!self || !self->open) co_return;
                self->tick();
                if (!SSL_net_write_desired(self->ssl)) { self->write_waiting = false; co_return; }
            } catch (...) {
                if (auto self = weak.lock(); self && self->open) {
                    self->failure = std::current_exception(); self->close();
                }
                co_return;
            }
        }
    }
    static task<void> timer_pump(std::weak_ptr<engine> weak) {
        for (;;) {
            io_context* io = nullptr;
#if defined(__linux__)
            int timer = -1;
#else
            clock::duration delay;
#endif
            { auto self = weak.lock(); if (!self || !self->open) co_return;
              io = &self->io;
#if defined(__linux__)
              timer = self->timer;
#else
              // Portable fallback until the runtime offers cancellable timers.
              // Linux uses rearmable timerfd and has no periodic polling.
              delay = std::clamp(self->timeout(), clock::duration(1ms), clock::duration(10ms));
#endif
            }
            try {
#if defined(__linux__)
                co_await io->wait_readable(timer);
                std::uint64_t expirations;
                const auto count = ::read(timer, &expirations, sizeof(expirations));
                if (count < 0 && errno != EAGAIN && errno != EINTR)
                    throw std::system_error(errno, std::generic_category(), "QUIC timer read");
#else
                co_await io->sleep_for(delay);
#endif
                auto self = weak.lock(); if (!self || !self->open) co_return;
                self->tick();
            } catch (...) {
                if (auto self = weak.lock(); self && self->open) {
                    self->failure = std::current_exception(); self->close();
                }
                co_return;
            }
        }
    }
};
struct connection_state {
    std::shared_ptr<engine> owner;
    SSL* ssl = nullptr;
    bool owned_ssl = false, connected = false, open = true, connecting = false, accepting = false, opening = false;
    ~connection_state() { if (owned_ssl) SSL_free(ssl); }
    void check(bool handshake = true) {
        owner->check();
        if (!open) throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        if (handshake && !connected) throw std::logic_error("QUIC handshake incomplete");
        SSL_CONN_CLOSE_INFO info{};
        if (SSL_get_conn_close_info(ssl, &info, sizeof(info))) throw std::system_error(std::make_error_code(std::errc::connection_aborted), "QUIC peer closed connection");
    }
};
struct stream_state {
    std::shared_ptr<connection_state> owner;
    SSL* ssl;
    bool reading = false, writing = false, reset = false;
    stream_state(std::shared_ptr<connection_state> parent, SSL* value) : owner(std::move(parent)), ssl(value) {}
    ~stream_state() { SSL_free(ssl); }
    void check() { owner->check(); if (reset) throw std::system_error(std::make_error_code(std::errc::operation_canceled)); }
};
} // namespace detail
namespace {
void start(const std::shared_ptr<detail::engine>& engine) {
    if (!engine->started) {
        engine->started = true;
        engine->io.spawn(detail::engine::read_pump(engine));
        engine->io.spawn(detail::engine::timer_pump(engine));
        engine->refresh();
    }
}
void configure_connection(SSL* ssl) {
    required(SSL_set_blocking_mode(ssl, 0), "QUIC nonblocking");
    required(SSL_set_default_stream_mode(ssl, SSL_DEFAULT_STREAM_MODE_NONE), "QUIC explicit streams");
    required(SSL_set_incoming_stream_policy(ssl, SSL_INCOMING_STREAM_POLICY_ACCEPT, 0), "QUIC accept stream policy");
    SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
}
task<void> handshake(std::shared_ptr<detail::connection_state> state, std::chrono::milliseconds timeout) {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(clock::time_point::max() - now);
    const auto deadline = timeout <= 0ms ? now : timeout >= maximum ? clock::time_point::max() : now + timeout;
    for (;;) {
        state->check(false); ERR_clear_error();
        const int result = SSL_do_handshake(state->ssl);
        if (result == 1) {
            // Only the engine reads UDP after the handshake. An unrelated
            // stream read must not consume packets without waking its peers.
            required(SSL_set_event_handling_mode(state->ssl, SSL_VALUE_EVENT_HANDLING_MODE_EXPLICIT), "QUIC explicit event processing");
            state->connected = true; state->owner->refresh(); co_return;
        }
        if (!retry(state->ssl, result)) fail("QUIC handshake");
        if (std::chrono::steady_clock::now() >= deadline) throw std::system_error(std::make_error_code(std::errc::timed_out), "QUIC handshake");
        co_await detail::engine::event{*state->owner, deadline};
    }
}
std::optional<std::size_t> read(detail::stream_state& state, std::span<char> buffer) {
    state.check(); if (buffer.empty()) return 0;
    ERR_clear_error(); size_t count = 0; const int result = SSL_read_ex(state.ssl, buffer.data(), buffer.size(), &count);
    const int error = result == 1 ? SSL_ERROR_NONE : SSL_get_error(state.ssl, result);
    if (result == 1) { state.owner->owner->schedule_tick(); return count; }
    if (error == SSL_ERROR_ZERO_RETURN) return 0;
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) return std::nullopt;
    fail("QUIC stream read");
}
std::size_t write(detail::stream_state& state, std::string_view bytes) {
    state.check(); if (bytes.empty()) return 0;
    ERR_clear_error(); size_t count = 0; const int result = SSL_write_ex(state.ssl, bytes.data(), bytes.size(), &count);
    const bool again = result != 1 && retry(state.ssl, result);
    if (result == 1) { state.owner->owner->schedule_tick(); return count; }
    if (again) return 0;
    fail("QUIC stream write");
}
} // namespace
context::context(role mode) : state_(std::make_shared<detail::configuration>(mode)) { set_alpn_protocols({"pace-quic"}); }
void context::load_verify_file(const std::string& path) { required(SSL_CTX_load_verify_locations(state_->ssl, path.c_str(), nullptr), "QUIC trust file"); }
void context::use_certificate_chain(const std::string& cert, const std::string& key) {
    required(SSL_CTX_use_certificate_chain_file(state_->ssl, cert.c_str()), "QUIC certificate");
    required(SSL_CTX_use_PrivateKey_file(state_->ssl, key.c_str(), SSL_FILETYPE_PEM), "QUIC private key");
    required(SSL_CTX_check_private_key(state_->ssl), "QUIC certificate/key mismatch");
}
void context::set_alpn_protocols(const std::vector<std::string>& protocols) {
    std::vector<unsigned char> encoded;
    for (const auto& value : protocols) {
        if (value.empty() || value.size() > 255) throw std::invalid_argument("invalid QUIC ALPN");
        encoded.push_back(static_cast<unsigned char>(value.size())); encoded.insert(encoded.end(), value.begin(), value.end());
    }
    if (encoded.empty() || encoded.size() > 65535) throw std::invalid_argument("QUIC requires ALPN");
    if (state_->mode == role::client && SSL_CTX_set_alpn_protos(state_->ssl, encoded.data(), static_cast<unsigned>(encoded.size())) != 0) fail("QUIC ALPN");
    state_->alpn = std::move(encoded);
}
connection::connection(io_context& io, const quic::context& config) : state_(std::make_shared<detail::connection_state>()) {
    if (config.state_->mode != role::client) throw std::invalid_argument("QUIC client requires client configuration");
    state_->owner = std::make_shared<detail::engine>(io, udp::endpoint{"0.0.0.0", 0}, config.state_);
    state_->owner->ssl = SSL_new(config.state_->ssl); state_->ssl = state_->owner->ssl;
    if (!state_->ssl) fail("QUIC connection");
    set_bio(state_->ssl, state_->owner->socket); configure_connection(state_->ssl);
}
connection::connection(std::shared_ptr<detail::connection_state> state) : state_(std::move(state)) {}
connection::connection(connection&&) noexcept = default;
connection& connection::operator=(connection&&) noexcept = default;
connection::~connection() = default;
io_context& connection::context() const noexcept { return state_->owner->io; }
bool connection::is_open() const noexcept {
    if (!state_ || !state_->open || !state_->owner->open || state_->owner->io.stopped()) return false;
    SSL_CONN_CLOSE_INFO info{}; return !SSL_get_conn_close_info(state_->ssl, &info, sizeof(info));
}
std::string connection::selected_protocol() const {
    const unsigned char* bytes = nullptr; unsigned size = 0; SSL_get0_alpn_selected(state_->ssl, &bytes, &size);
    return size ? std::string(reinterpret_cast<const char*>(bytes), size) : std::string{};
}
void connection::close(std::uint64_t code) noexcept {
    // Waking an operation may destroy its public connection/stream handle.
    // Retain the state until every cancellation callback has unwound.
    auto state = state_;
    if (!state || !state->open) return;
    SSL_SHUTDOWN_EX_ARGS args{code, nullptr};
    SSL_shutdown_ex(state->ssl, SSL_SHUTDOWN_FLAG_RAPID | SSL_SHUTDOWN_FLAG_NO_BLOCK, &args, sizeof(args));
    SSL_handle_events(state->owner->ssl);
    state->open = false;
    if (!state->owned_ssl) state->owner->close();
    else state->owner->notify();
}
task<void> connection::connect(udp::endpoint peer, std::string name, std::chrono::milliseconds timeout) {
    auto state = state_; guard operation(state->connecting);
    state->check(false);
    if (state->connected || state->owned_ssl) throw std::logic_error("QUIC connect state");
    try {
        if (name.empty() || name.find('\0') != std::string::npos) throw std::invalid_argument("QUIC client requires certificate identity");
        const auto remote = pace::detail::parse_socket_address(peer.address, peer.port, peer.family, peer.scope_id);
        if (!peer.port) throw std::invalid_argument("QUIC remote port must be nonzero");
        // BIO_ADDR_rawmake cannot carry an IPv6 interface scope. Reject rather
        // than silently sending a link-local destination on the wrong interface.
        if (peer.scope_id) throw std::invalid_argument("scoped QUIC destinations are not supported");
        if (remote.family() == address_family::ipv6) {
            state->owner->socket = udp::socket(state->owner->io, udp::endpoint{"::", 0});
            set_bio(state->ssl, state->owner->socket);
        }
        std::unique_ptr<BIO_ADDR, decltype(&BIO_ADDR_free)> addr(BIO_ADDR_new(), BIO_ADDR_free);
        if (!addr) fail("QUIC peer allocation");
        if (remote.family() == address_family::ipv6) {
            sockaddr_in6 v6{};
            std::memcpy(&v6, &remote.storage, sizeof(v6));
            required(BIO_ADDR_rawmake(addr.get(), AF_INET6, &v6.sin6_addr, sizeof(v6.sin6_addr), v6.sin6_port), "QUIC peer address");
        } else {
            sockaddr_in v4{};
            std::memcpy(&v4, &remote.storage, sizeof(v4));
            required(BIO_ADDR_rawmake(addr.get(), AF_INET, &v4.sin_addr, sizeof(v4.sin_addr), v4.sin_port), "QUIC peer address");
        }
        required(SSL_set1_initial_peer_addr(state->ssl, addr.get()), "QUIC initial peer");
        in_addr ip{}; in6_addr ip6{};
        if (inet_pton(AF_INET, name.c_str(), &ip) == 1 || inet_pton(AF_INET6, name.c_str(), &ip6) == 1)
            required(X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(state->ssl), name.c_str()), "QUIC peer IP");
        else { required(SSL_set_tlsext_host_name(state->ssl, name.c_str()), "QUIC SNI"); required(SSL_set1_host(state->ssl, name.c_str()), "QUIC hostname"); }
        start(state->owner); co_await handshake(state, timeout);
    } catch (...) { close(); throw; }
}
namespace {
std::shared_ptr<detail::stream_state> new_stream(const std::shared_ptr<detail::connection_state>& state, stream_type type) {
    state->check();
    if (type != stream_type::bidirectional && type != stream_type::unidirectional) throw std::invalid_argument("QUIC stream type");
    ERR_clear_error();
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new_stream(state->ssl,
        SSL_STREAM_FLAG_NO_BLOCK | (type == stream_type::unidirectional ? SSL_STREAM_FLAG_UNI : 0)), SSL_free);
    if (!ssl) {
        const auto code = ERR_peek_last_error();
        if (code && ERR_GET_REASON(code) != SSL_R_STREAM_COUNT_LIMITED) fail("QUIC open stream");
        return {};
    }
    auto result = std::make_shared<detail::stream_state>(state, ssl.get());
    ssl.release(); return result;
}
std::shared_ptr<detail::stream_state> accept_stream(const std::shared_ptr<detail::connection_state>& state) {
    state->check(); ERR_clear_error();
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_accept_stream(state->ssl, SSL_ACCEPT_STREAM_NO_BLOCK), SSL_free);
    if (!ssl) { if (ERR_peek_error()) fail("QUIC accept stream"); return {}; }
    auto result = std::make_shared<detail::stream_state>(state, ssl.get());
    ssl.release(); return result;
}
}
task<stream> connection::open_stream(stream_type type) {
    auto state = state_; guard operation(state->opening);
    for (;;) {
        if (auto result = new_stream(state, type)) co_return stream(std::move(result));
        co_await detail::engine::event{*state->owner};
    }
}
std::optional<stream> connection::try_open_stream(stream_type type) {
    guard operation(state_->opening);
    if (auto result = new_stream(state_, type)) return stream(std::move(result));
    return std::nullopt;
}
std::optional<stream> connection::try_accept_stream() {
    guard operation(state_->accepting);
    if (auto result = quic::accept_stream(state_)) return stream(std::move(result));
    return std::nullopt;
}
task<stream> connection::accept_stream() {
    auto state = state_; guard operation(state->accepting);
    for (;;) {
        if (auto result = quic::accept_stream(state)) co_return stream(std::move(result));
        co_await detail::engine::event{*state->owner};
    }
}
stream::stream(std::shared_ptr<detail::stream_state> state) : state_(std::move(state)) {}
stream::stream(stream&&) noexcept = default;
stream& stream::operator=(stream&&) noexcept = default;
stream::~stream() = default;
std::uint64_t stream::id() const noexcept { return SSL_get_stream_id(state_->ssl); }
bool stream::is_unidirectional() const noexcept { return (id() & 2) != 0; }
bool stream::is_local() const noexcept { return SSL_is_stream_local(state_->ssl) != 0; }
io_context& stream::context() const noexcept { return state_->owner->owner->io; }
std::optional<std::size_t> stream::try_read(std::span<char> bytes) { guard operation(state_->reading); return read(*state_, bytes); }
std::size_t stream::try_write(std::string_view bytes) { guard operation(state_->writing); return write(*state_, bytes); }
task<std::size_t> stream::read_some(std::span<char> bytes) {
    auto state = state_; guard operation(state->reading);
    for (;;) { if (auto count = read(*state, bytes)) co_return *count; co_await detail::engine::event{*state->owner->owner}; }
}
task<void> stream::write_all(std::string_view bytes) {
    auto state = state_; guard operation(state->writing);
    while (!bytes.empty()) { const auto count = write(*state, bytes); bytes.remove_prefix(count); if (!count) co_await detail::engine::event{*state->owner->owner}; }
}
task<void> stream::wait_events() {
    auto state = state_; state->check();
    co_await detail::engine::event{*state->owner->owner};
    state->check();
}
task<void> connection::wait_events() {
    auto state = state_; state->check();
    co_await detail::engine::event{*state->owner};
    state->check();
}
std::uint64_t stream::pending_write_bytes() const {
    state_->check(); std::uint64_t bytes = 0;
    required(SSL_get_stream_write_buf_used(state_->ssl, &bytes), "QUIC write buffer accounting"); return bytes;
}
void stream::finish_write() { guard operation(state_->writing); state_->check(); required(SSL_stream_conclude(state_->ssl, 0), "QUIC stream FIN"); state_->owner->owner->schedule_tick(); }
void stream::reset(std::uint64_t code) noexcept {
    auto state = state_;
    if (!state) return;
    SSL_STREAM_RESET_ARGS args{code}; SSL_stream_reset(state->ssl, &args, sizeof(args)); state->reset = true;
    try { state->owner->owner->schedule_tick(); }
    catch (...) { state->owner->owner->failure = std::current_exception(); state->owner->owner->close(); }
    state->owner->owner->notify();
}
listener::listener(io_context& io, udp::endpoint local, const quic::context& config) {
    if (config.state_->mode != role::server) throw std::invalid_argument("QUIC listener requires server configuration");
    state_ = std::make_shared<detail::engine>(io, std::move(local), config.state_);
    state_->ssl = SSL_new_listener(config.state_->ssl, 0); if (!state_->ssl) fail("QUIC listener");
    set_bio(state_->ssl, state_->socket); required(SSL_listen(state_->ssl), "QUIC listen"); start(state_);
}
listener::~listener() { close(); }
void listener::close() noexcept { auto state = state_; if (state) state->close(); }
udp::endpoint listener::local_endpoint() const { return state_->socket.local_endpoint(); }
io_context& listener::context() const noexcept { return state_->io; }
bool listener::is_open() const noexcept { return state_ && state_->open && !state_->io.stopped(); }
task<connection> listener::accept(std::chrono::milliseconds timeout) {
    auto engine = state_; guard operation(engine->accepting);
    for (;;) {
        engine->check(); ERR_clear_error();
        auto* ssl = SSL_get_accept_connection_queue_len(engine->ssl) ? SSL_accept_connection(engine->ssl, SSL_ACCEPT_CONNECTION_NO_BLOCK) : nullptr;
        if (ssl) {
            std::unique_ptr<SSL, decltype(&SSL_free)> accepted(ssl, SSL_free);
            auto state = std::make_shared<detail::connection_state>(); state->owner = engine; state->ssl = accepted.release(); state->owned_ssl = true;
            configure_connection(ssl); co_await handshake(state, timeout); co_return connection(std::move(state));
        }
        if (ERR_peek_error()) fail("QUIC accept connection");
        co_await detail::engine::event{*engine};
    }
}
} // namespace pace::quic
#endif
