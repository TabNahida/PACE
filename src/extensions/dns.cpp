#include <pace/dns.hpp>
#ifdef PACE_HAS_DNS
#include <ares.h>
#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include <algorithm>
#include <climits>
#include <coroutine>
#include <exception>
#include <map>
#include <stdexcept>
#include <utility>

namespace pace::dns {
namespace {
class category final : public std::error_category {
public:
    const char* name() const noexcept override { return "pace.dns"; }
    std::string message(int value) const override { return ares_strerror(value); }
};
void check_status(int status) {
    if (status != ARES_SUCCESS) throw std::system_error(status, error_category(), "DNS resolver");
}
void initialize() {
    detail::initialize_sockets();
    struct library {
        library() { check_status(ares_library_init(ARES_LIB_INIT_ALL)); }
        ~library() { ares_library_cleanup(); }
    };
    static library instance;
    (void)instance;
}
struct lookup : std::enable_shared_from_this<lookup> {
    struct socket_state {
        ares_socket_t fd;
        bool alive = true, readable = false, writable = false;
        bool reader = false, writer = false;
    };
    io_context& io;
    ares_channel_t* channel = nullptr;
    std::map<ares_socket_t, std::shared_ptr<socket_state>> sockets;
    std::chrono::steady_clock::time_point deadline;
    result value;
    std::exception_ptr failure;
    std::error_code error;
    std::coroutine_handle<> continuation;
    bool done = false, cleaning = false;
    explicit lookup(io_context& context) : io(context) {}
    ~lookup() { cleanup(); }
    void cleanup() noexcept {
        if (cleaning) return;
        cleaning = true;
        for (auto& [fd, state] : sockets) {
            state->alive = false;
            io.cancel(static_cast<native_socket_type>(fd));
        }
        if (channel) { auto* old = std::exchange(channel, nullptr); ares_destroy(old); }
        sockets.clear();
        cleaning = false;
    }
    void finish() noexcept {
        if (!done && !failure) return;
        done = true;
        cleanup();
        // Resume only after leaving c-ares: callbacks never resume user code.
        if (auto waiting = std::exchange(continuation, {})) waiting.resume();
    }
    void cancel(std::errc reason) noexcept {
        if (done) return;
        error = std::make_error_code(reason); done = true;
        finish();
    }
    static void socket_changed(void* pointer, ares_socket_t fd, int read, int write) noexcept {
        auto& self = *static_cast<lookup*>(pointer);
        if (self.cleaning) return;
        try {
            auto found = self.sockets.find(fd);
            if (!read && !write) {
                if (found != self.sockets.end()) {
                    found->second->alive = false;
                    self.io.cancel(static_cast<native_socket_type>(fd));
                    self.sockets.erase(found);
                }
                return;
            }
            if (found == self.sockets.end()) {
                auto state = std::make_shared<socket_state>(); state->fd = fd;
                found = self.sockets.emplace(fd, std::move(state)).first;
            }
            auto& state = *found->second;
            if ((state.readable && !read) || (state.writable && !write))
                self.io.cancel(static_cast<native_socket_type>(fd));
            state.readable = read != 0; state.writable = write != 0;
        } catch (...) { self.failure = std::current_exception(); }
    }
    static void resolved(void* pointer, int status, int, ares_addrinfo* addresses) noexcept {
        auto& self = *static_cast<lookup*>(pointer);
        if (!self.done) {
            try {
                if (status == ARES_ECANCELLED || status == ARES_EDESTRUCTION)
                    self.error = std::make_error_code(std::errc::operation_canceled);
                else if (status != ARES_SUCCESS) self.error = {status, error_category()};
                else {
                    if (addresses && addresses->name) self.value.canonical_name = addresses->name;
                    for (auto* node = addresses ? addresses->nodes : nullptr; node; node = node->ai_next) {
                        char text[INET6_ADDRSTRLEN]{};
                        const void* bytes = nullptr;
                        family type;
                        if (node->ai_family == AF_INET) {
                            type = family::ipv4;
                            bytes = &reinterpret_cast<const sockaddr_in*>(node->ai_addr)->sin_addr;
                        } else if (node->ai_family == AF_INET6) {
                            type = family::ipv6;
                            bytes = &reinterpret_cast<const sockaddr_in6*>(node->ai_addr)->sin6_addr;
                        } else continue;
                        if (!ares_inet_ntop(node->ai_family, bytes, text, sizeof(text)))
                            throw std::runtime_error("DNS address conversion failed");
                        if (std::none_of(self.value.addresses.begin(), self.value.addresses.end(),
                            [&](const address& item) { return item.type == type && item.text == text; }))
                            self.value.addresses.push_back({type, text});
                    }
                    if (self.value.addresses.empty()) self.error = {ARES_ENODATA, error_category()};
                }
            } catch (...) { self.failure = std::current_exception(); }
            self.done = true;
        }
        if (addresses) ares_freeaddrinfo(addresses);
    }
    void arm() {
        if (done || failure) { finish(); return; }
        for (auto& [fd, state] : sockets) {
            (void)fd;
            if (state->readable && !state->reader) {
                state->reader = true;
                try { io.spawn(watch(shared_from_this(), state, false)); }
                catch (...) { state->reader = false; throw; }
            }
            if (state->writable && !state->writer) {
                state->writer = true;
                try { io.spawn(watch(shared_from_this(), state, true)); }
                catch (...) { state->writer = false; throw; }
            }
        }
    }
    void process(ares_socket_t read, ares_socket_t write) noexcept {
        if (done) return;
        try {
            ares_process_fd(channel, read, write);
            if (!done && !failure) arm();
        } catch (...) { failure = std::current_exception(); }
        finish();
    }
    static task<void> watch(std::shared_ptr<lookup> self, std::shared_ptr<socket_state> socket, bool writable) {
        auto& active = writable ? socket->writer : socket->reader;
        struct reset { bool& value; ~reset() { value = false; } } guard{active};
        try {
            while (!self->done && socket->alive && (writable ? socket->writable : socket->readable)) {
                try {
                    if (writable) co_await self->io.wait_writable(static_cast<native_socket_type>(socket->fd));
                    else co_await self->io.wait_readable(static_cast<native_socket_type>(socket->fd));
                } catch (const std::system_error& e) {
                    if (e.code() != std::errc::operation_canceled) throw;
                    if (self->io.stopped()) self->cancel(std::errc::operation_canceled);
                    continue;
                }
                if (self->done || !socket->alive) break;
                self->process(writable ? ARES_SOCKET_BAD : socket->fd, writable ? socket->fd : ARES_SOCKET_BAD);
            }
        } catch (...) { self->failure = std::current_exception(); self->finish(); }
    }
    static task<void> timer(std::shared_ptr<lookup> self) {
        try {
            while (!self->done) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= self->deadline) { self->cancel(std::errc::timed_out); break; }
                // This coroutine also propagates context shutdown to lookups.
                // A short cap bounds retention of completed operation state.
                timeval maximum{0, 100000}, next{};
                const auto* wait = ares_timeout(self->channel, &maximum, &next);
                auto delay = std::chrono::microseconds(wait->tv_sec * 1000000LL + wait->tv_usec);
                delay = std::min(delay, std::chrono::duration_cast<std::chrono::microseconds>(self->deadline - now));
                co_await self->io.sleep_for(std::max(delay, std::chrono::microseconds(1)));
                if (!self->done) {
                    if (std::chrono::steady_clock::now() >= self->deadline) self->cancel(std::errc::timed_out);
                    else self->process(ARES_SOCKET_BAD, ARES_SOCKET_BAD);
                }
            }
        } catch (const std::system_error& e) {
            if (e.code() == std::errc::operation_canceled) self->cancel(std::errc::operation_canceled);
            else { self->failure = std::current_exception(); self->finish(); }
        } catch (...) { self->failure = std::current_exception(); self->finish(); }
    }
    struct completion {
        lookup& state;
        io_context::cancellation_handle registration{};
        bool await_ready() const noexcept { return state.done; }
        template<class P> void await_suspend(std::coroutine_handle<P> continuation) {
            if constexpr (requires { continuation.promise().cancellation; }) {
                registration = state.io.on_cancel(continuation.promise().cancellation,
                    [owner = state.shared_from_this()](std::error_code error) {
                        owner->cancel(error == std::errc::timed_out ? std::errc::timed_out : std::errc::operation_canceled);
                    });
            }
            state.continuation = continuation;
        }
        void await_resume() const noexcept {}
    };
};
} // namespace
const std::error_category& error_category() noexcept { static category instance; return instance; }
struct resolver::implementation {
    io_context& io;
    options configuration;
    std::weak_ptr<lookup> active;
    bool busy = false, closed = false;
    implementation(io_context& context, options value) : io(context), configuration(std::move(value)) {}
};
resolver::resolver(io_context& io, options configuration)
    : impl_(std::make_shared<implementation>(io, std::move(configuration))) {
    const auto& value = impl_->configuration;
    if (value.timeout.count() <= 0 || value.timeout > std::chrono::hours(24) || value.tries == 0 || value.tries > 10 ||
        value.servers.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid DNS resolver options");
    initialize();
}
resolver::~resolver() { impl_->closed = true; cancel(); }
io_context& resolver::context() const noexcept { return impl_->io; }
void resolver::cancel() noexcept {
    if (auto active = impl_->active.lock()) active->cancel(std::errc::operation_canceled);
}
task<result> resolver::resolve(std::string hostname, family requested) {
    return resolve_impl(impl_, std::move(hostname), requested);
}
task<result> resolver::resolve_impl(std::shared_ptr<implementation> state, std::string hostname, family requested) {
    if (io_context::current() != &state->io) throw std::logic_error("DNS lookup must run on its owning io_context");
    if (state->closed || state->io.stopped()) throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    if (state->busy) throw std::logic_error("DNS resolver permits one active lookup");
    if (hostname.empty() || hostname.size() > 253 || hostname.find('\0') != std::string::npos ||
        std::any_of(hostname.begin(), hostname.end(), [](unsigned char c) { return c <= 32 || c == 127; }) ||
        (requested != family::any && requested != family::ipv4 && requested != family::ipv6))
        throw std::invalid_argument("invalid DNS hostname/family");
    state->busy = true;
    struct reset { implementation& state; ~reset() { state.active.reset(); state.busy = false; } } guard{*state};
    auto operation = std::make_shared<lookup>(state->io);
    state->active = operation;
    try {
        ares_options config{};
        config.sock_state_cb = lookup::socket_changed;
        config.sock_state_cb_data = operation.get();
        config.timeout = static_cast<int>(std::min<long long>(1000, state->configuration.timeout.count()));
        config.tries = static_cast<int>(state->configuration.tries);
        // Exact names avoid implicit search-domain traffic; hosts-file lookup
        // still follows the system configuration. Disable speculative sorting.
        config.flags = ARES_FLAG_NOSEARCH;
        check_status(ares_init_options(&operation->channel, &config,
            ARES_OPT_SOCK_STATE_CB | ARES_OPT_TIMEOUTMS | ARES_OPT_TRIES | ARES_OPT_FLAGS));
        if (!state->configuration.servers.empty())
            check_status(ares_set_servers_ports_csv(operation->channel, state->configuration.servers.c_str()));
        operation->deadline = std::chrono::steady_clock::now() + state->configuration.timeout;
        ares_addrinfo_hints hints{};
        hints.ai_family = requested == family::ipv4 ? AF_INET : requested == family::ipv6 ? AF_INET6 : AF_UNSPEC;
        hints.ai_flags = ARES_AI_NOSORT | ARES_AI_CANONNAME;
        ares_getaddrinfo(operation->channel, hostname.c_str(), nullptr, &hints, lookup::resolved, operation.get());
        if (!operation->done && !operation->failure) {
            operation->arm();
            state->io.spawn(lookup::timer(operation));
        }
        operation->finish();
        co_await lookup::completion{*operation};
        if (operation->failure) std::rethrow_exception(operation->failure);
        if (operation->error) throw std::system_error(operation->error, "resolve " + hostname);
        co_return std::move(operation->value);
    } catch (...) {
        operation->done = true; operation->cleanup(); throw;
    }
}
} // namespace pace::dns
#endif
