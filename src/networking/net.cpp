#include <pace/tcp.hpp>
#include <pace/this_context.hpp>
#include <pace/detail/profiling.hpp>
#include <pace/detail/socket_address.hpp>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace pace::tcp {
namespace {
#if defined(_WIN32)
using socket_length = int;
int last_socket_error() noexcept { return ::WSAGetLastError(); }
void close_socket(native_socket_type fd) noexcept { ::closesocket(fd); }
bool interrupted(int error) noexcept { return error == WSAEINTR; }
bool would_block(int error) noexcept { return error == WSAEWOULDBLOCK; }
#else
using socket_length = socklen_t;
int last_socket_error() noexcept { return errno; }
void close_socket(native_socket_type fd) noexcept { ::close(fd); }
bool interrupted(int error) noexcept { return error == EINTR; }
bool would_block(int error) noexcept { return error == EAGAIN || error == EWOULDBLOCK; }
#endif

std::error_code socket_error_code(int error) {
#if defined(_WIN32)
    return {error, std::system_category()};
#else
    return {error, std::generic_category()};
#endif
}

[[noreturn]] void socket_error(const char* what) {
    throw std::system_error(socket_error_code(last_socket_error()), what);
}

void configure_socket(native_socket_type fd) {
#if defined(_WIN32)
    u_long value = 1;
    if (::ioctlsocket(fd, FIONBIO, &value) != 0) socket_error("ioctlsocket(FIONBIO)");
#else
    if (::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0 ||
        ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) socket_error("fcntl(nonblocking)");
#if defined(SO_NOSIGPIPE)
    const int yes = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) < 0)
        socket_error("setsockopt(SO_NOSIGPIPE)");
#endif
#endif
}

int set_option(native_socket_type fd, int level, int name, int value) {
    return ::setsockopt(fd, level, name, reinterpret_cast<const char*>(&value), sizeof(value));
}

native_socket_type create_socket(address_family family) {
    detail::initialize_sockets();
    native_socket_type fd = ::socket(detail::native_address_family(family), SOCK_STREAM, 0);
    if (fd == invalid_socket) socket_error("socket");
    try {
        configure_socket(fd);
        // Outbound mapped IPv4 destinations work consistently across systems.
        // A listener overrides this before binding, defaulting to IPv6-only.
        if (family == address_family::ipv6 && set_option(fd, IPPROTO_IPV6, IPV6_V6ONLY, 0) < 0)
            socket_error("setsockopt(IPV6_V6ONLY)");
    }
    catch (...) { close_socket(fd); throw; }
    return fd;
}

void check_operation(io_context& context, native_socket_type fd) {
    if (context.stopped())
        throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    if (fd == invalid_socket)
        throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor));
    if (auto* running = io_context::current(); running != nullptr && running != &context)
        throw std::logic_error("socket operation used from a different io_context");
}
} // namespace

stream::stream(io_context& context) : stream(context, address_family::unspecified) {}
stream::stream(io_context& context, address_family family)
    : context_(&context), fd_(create_socket(family == address_family::unspecified ? address_family::ipv4 : family)),
      family_(family == address_family::unspecified ? address_family::ipv4 : family),
      automatic_family_(family == address_family::unspecified) {}
stream::stream(io_context& context, native_socket_type fd, address_family family) noexcept
    : context_(&context), fd_(fd), family_(family), automatic_family_(false), connect_started_(true) {}
stream::~stream() { close(); }
stream::stream(stream&& other) noexcept
    : read_may_be_ready_(other.read_may_be_ready_), context_(other.context_),
      fd_(std::exchange(other.fd_, invalid_socket)), family_(other.family_),
      automatic_family_(other.automatic_family_), connect_started_(other.connect_started_), no_delay_(other.no_delay_) {}
stream& stream::operator=(stream&& other) noexcept {
    if (this != &other) {
        close();
        read_may_be_ready_ = other.read_may_be_ready_;
        context_ = other.context_;
        fd_ = std::exchange(other.fd_, invalid_socket);
        family_ = other.family_;
        automatic_family_ = other.automatic_family_;
        connect_started_ = other.connect_started_;
        no_delay_ = other.no_delay_;
    }
    return *this;
}

task<void> stream::connect(endpoint endpoint) {
    check_operation(*context_, fd_);
    const auto address = detail::parse_socket_address(endpoint.address, endpoint.port, endpoint.family, endpoint.scope_id);
    if (connect_started_) throw std::logic_error("TCP connect may only be started once per stream");
    if (address.family() != family_) {
        if (!automatic_family_)
            throw std::system_error(std::make_error_code(std::errc::address_family_not_supported), "TCP connect family");
        const auto replacement = create_socket(address.family());
        try {
            if (no_delay_ && set_option(replacement, IPPROTO_TCP, TCP_NODELAY, 1) < 0)
                socket_error("setsockopt(TCP_NODELAY)");
        } catch (...) { close_socket(replacement); throw; }
        context_->cancel(fd_);
        close_socket(std::exchange(fd_, replacement));
        family_ = address.family();
    }
    connect_started_ = true;
    int result;
    do { result = ::connect(fd_, address.data(), address.length); }
    while (result < 0 && interrupted(last_socket_error()));
    if (result == 0) co_return;
    const auto pending_error = last_socket_error();
#if defined(_WIN32)
    if (pending_error != WSAEWOULDBLOCK && pending_error != WSAEINPROGRESS && pending_error != WSAEALREADY)
#else
    if (pending_error != EINPROGRESS && pending_error != EALREADY)
#endif
        socket_error("connect");
    co_await context_->wait_writable(fd_);
    check_operation(*context_, fd_);
    int error = 0;
    socket_length length = sizeof(error);
    if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) < 0)
        socket_error("getsockopt(SO_ERROR)");
    if (error != 0) throw std::system_error(socket_error_code(error), "connect");
}

task<std::size_t> stream::read_some(std::span<char> buffer) {
    check_operation(*context_, fd_);
    if (buffer.empty()) co_return 0;
    if (!read_may_be_ready_) co_await context_->wait_readable(fd_);
    for (;;) {
        check_operation(*context_, fd_);
        if (buffer.empty()) co_return 0;
        const auto count = detail::profiling::measure(detail::profiling::stage::recv, [&] { return ::recv(fd_, buffer.data(), static_cast<int>(std::min<std::size_t>(buffer.size(), INT_MAX)), 0); });
        if (count >= 0) {
            read_may_be_ready_ = static_cast<std::size_t>(count) == std::min<std::size_t>(buffer.size(), INT_MAX);
            co_return static_cast<std::size_t>(count);
        }
        const auto error = last_socket_error();
        if (interrupted(error)) continue;
        if (!would_block(error)) socket_error("recv");
        co_await context_->wait_readable(fd_);
    }
}

task<void> stream::write_all(std::string_view bytes) {
    do {
        check_operation(*context_, fd_);
        if (bytes.empty()) co_return;
#if defined(MSG_NOSIGNAL)
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const auto count = detail::profiling::measure(detail::profiling::stage::send, [&] { return ::send(fd_, bytes.data(), static_cast<int>(std::min<std::size_t>(bytes.size(), INT_MAX)), flags); });
        if (count > 0) {
            bytes.remove_prefix(static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0)
            throw std::system_error(std::make_error_code(std::errc::broken_pipe));
        const auto error = last_socket_error();
        if (interrupted(error)) continue;
        if (!would_block(error)) socket_error("send");
        co_await context_->wait_writable(fd_);
    } while (!bytes.empty());
}

void stream::set_no_delay(bool enabled) {
    check_operation(*context_, fd_);
    const int value = enabled ? 1 : 0;
    if (set_option(fd_, IPPROTO_TCP, TCP_NODELAY, value) < 0)
        socket_error("setsockopt(TCP_NODELAY)");
    no_delay_ = enabled;
}

endpoint stream::local_endpoint() const {
    check_operation(*context_, fd_);
    detail::socket_address address;
    if (::getsockname(fd_, address.data(), &address.length) < 0) socket_error("getsockname");
    return detail::socket_endpoint<endpoint>(address.data(), address.length);
}

endpoint stream::remote_endpoint() const {
    check_operation(*context_, fd_);
    detail::socket_address address;
    if (::getpeername(fd_, address.data(), &address.length) < 0) socket_error("getpeername");
    return detail::socket_endpoint<endpoint>(address.data(), address.length);
}

task<void> stream::close_gracefully(std::chrono::milliseconds timeout, std::size_t discard_limit) {
    try {
        check_operation(*context_, fd_);
#if defined(_WIN32)
        constexpr int send_side = SD_SEND;
#else
        constexpr int send_side = SHUT_WR;
#endif
        int result;
        do { result = ::shutdown(fd_, send_side); }
        while (result < 0 && interrupted(last_socket_error()));
        if (result < 0) socket_error("shutdown(send)");
        if (timeout.count() <= 0 || discard_limit == 0) { close(); co_return; }

        const auto started = std::chrono::steady_clock::now();
        const auto allowed_time = std::chrono::duration<long double>(timeout);
        bool expired = false;
        auto deadline = context_->call_after(timeout, [this, &expired] {
            expired = true;
            close();
        });
        std::array<char, 4096> discarded{};
        std::size_t total = 0;
        while (!expired && total < discard_limit &&
               std::chrono::duration<long double>(std::chrono::steady_clock::now() - started) < allowed_time) {
            check_operation(*context_, fd_);
            const auto wanted = static_cast<int>(std::min(discarded.size(), discard_limit - total));
            const auto count = detail::profiling::measure(detail::profiling::stage::recv, [&] { return ::recv(fd_, discarded.data(), wanted, 0); });
            if (count == 0) break;
            if (count > 0) {
                total += static_cast<std::size_t>(count);
                continue;
            }
            const auto error = last_socket_error();
            if (interrupted(error)) continue;
            if (!would_block(error)) socket_error("recv during graceful close");
            try {
                co_await context_->wait_readable(fd_);
            } catch (const std::system_error&) {
                // Timeout closes and cancels the readiness wait. If readiness
                // was already queued, the loop's expiry check avoids using the
                // closed handle; an independent context stop still propagates.
                if (expired && !context_->stopped()) break;
                throw;
            }
        }
        if (context_->stopped())
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    } catch (...) {
        close();
        throw;
    }
    close();
}

void stream::close() noexcept {
    if (fd_ != invalid_socket) {
        context_->cancel(fd_);
        close_socket(std::exchange(fd_, invalid_socket));
    }
}

listener::listener(io_context& context, endpoint endpoint, listen_options options)
    : context_(&context) {
    const auto address = detail::parse_socket_address(endpoint.address, endpoint.port, endpoint.family, endpoint.scope_id);
    if (options.backlog <= 0) throw std::invalid_argument("listen backlog must be positive");
    if (address.family() == address_family::ipv4 && !options.ipv6_only)
        throw std::invalid_argument("dual-stack listen requires an IPv6 endpoint");
    family_ = address.family();
    fd_ = create_socket(family_);
    try {
        if (family_ == address_family::ipv6 &&
            set_option(fd_, IPPROTO_IPV6, IPV6_V6ONLY, options.ipv6_only ? 1 : 0) < 0)
            socket_error("setsockopt(IPV6_V6ONLY)");
#if defined(_WIN32)
        if (set_option(fd_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1) < 0)
            socket_error("setsockopt(SO_EXCLUSIVEADDRUSE)");
#else
        if (set_option(fd_, SOL_SOCKET, SO_REUSEADDR, 1) < 0)
            socket_error("setsockopt(SO_REUSEADDR)");
#endif
#if defined(SO_REUSEPORT_LB)
        if (options.reuse_port &&
            set_option(fd_, SOL_SOCKET, SO_REUSEPORT_LB, 1) < 0)
            socket_error("setsockopt(SO_REUSEPORT_LB)");
#elif defined(SO_REUSEPORT)
        if (options.reuse_port &&
            set_option(fd_, SOL_SOCKET, SO_REUSEPORT, 1) < 0)
            socket_error("setsockopt(SO_REUSEPORT)");
#else
        if (options.reuse_port)
            throw std::system_error(std::make_error_code(std::errc::operation_not_supported), "SO_REUSEPORT");
#endif
        if (::bind(fd_, address.data(), address.length) < 0)
            socket_error("bind");
        if (::listen(fd_, options.backlog) < 0) socket_error("listen");
    } catch (...) {
        close_socket(std::exchange(fd_, invalid_socket));
        throw;
    }
}

listener::~listener() { close(); }
listener::listener(listener&& other) noexcept
    : context_(other.context_), fd_(std::exchange(other.fd_, invalid_socket)), family_(other.family_) {}
listener& listener::operator=(listener&& other) noexcept {
    if (this != &other) {
        close();
        context_ = other.context_;
        fd_ = std::exchange(other.fd_, invalid_socket);
        family_ = other.family_;
    }
    return *this;
}

task<stream> listener::accept() {
    for (;;) {
        check_operation(*context_, fd_);
        const native_socket_type accepted = detail::profiling::measure(detail::profiling::stage::accept, [&] { return ::accept(fd_, nullptr, nullptr); });
        if (accepted != invalid_socket) {
            try { configure_socket(accepted); }
            catch (...) { close_socket(accepted); throw; }
            co_return stream(*context_, accepted, family_);
        }
        const auto error = last_socket_error();
        if (interrupted(error)) continue;
#if defined(_WIN32)
        if (error == WSAECONNRESET) continue;
#else
        if (error == ECONNABORTED || error == EPROTO) continue;
#endif
        if (!would_block(error)) socket_error("accept");
        co_await context_->wait_readable(fd_);
    }
}

endpoint listener::local_endpoint() const {
    check_operation(*context_, fd_);
    detail::socket_address address;
    if (::getsockname(fd_, address.data(), &address.length) < 0)
        socket_error("getsockname");
    return detail::socket_endpoint<endpoint>(address.data(), address.length);
}

void listener::close() noexcept {
    if (fd_ != invalid_socket) {
        context_->cancel(fd_);
        close_socket(std::exchange(fd_, invalid_socket));
    }
}

task<stream> connect(io_context& context, endpoint destination) {
    stream socket(context);
    co_await socket.connect(std::move(destination));
    co_return socket;
}

task<stream> connect(endpoint destination) {
    co_return co_await connect(this_context(), std::move(destination));
}

task<void> stream::read_exact(std::span<char> buffer) {
    while (!buffer.empty()) {
        const auto size = co_await read_some(buffer);
        if (!size) throw std::system_error(std::make_error_code(std::errc::connection_reset), "EOF before read_exact completed");
        buffer = buffer.subspan(size);
    }
}

task<std::string> stream::read_exact(std::size_t size, std::size_t maximum) {
    if (size > maximum) throw std::length_error("read_exact exceeds maximum");
    std::string buffer(size, '\0');
    co_await read_exact(std::span<char>(buffer));
    co_return buffer;
}
} // namespace pace::tcp
