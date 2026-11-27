#include <pace/net/tcp.hpp>
#include <pace/runtime/operation.hpp>
#include <pace/runtime/this_context.hpp>
#include "net/socket_address.hpp"
#include "net/socket_ops.hpp"
#include "runtime/profiling.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
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

namespace pace::tcp
{
namespace
{
using net::close_socket;
using net::interrupted;
using net::last_socket_error;
using net::socket_error_code;
using net::socket_length;
using net::would_block;

[[noreturn]] void socket_error(const char* what)
{
    net::throw_socket_error(what);
}

void configure_socket(native_socket_type fd)
{
#if defined(_WIN32)
    u_long value = 1;
    if (::ioctlsocket(fd, FIONBIO, &value) != 0)
    {
        socket_error("ioctlsocket(FIONBIO)");
    }
#else
    if (::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0 || ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
    {
        socket_error("fcntl(nonblocking)");
    }
#if defined(SO_NOSIGPIPE)
    const int yes = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) < 0)
    {
        socket_error("setsockopt(SO_NOSIGPIPE)");
    }
#endif
#endif
}

native_socket_type accept_socket(native_socket_type listener)
{
#if defined(__linux__)
    const auto fast = ::accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fast != invalid_socket || errno != ENOSYS)
    {
        return fast;
    }
#endif
    const auto fd = ::accept(listener, nullptr, nullptr);
    if (fd != invalid_socket)
    {
        try
        {
#if defined(_WIN32)
            if (!::SetHandleInformation(reinterpret_cast<HANDLE>(fd), HANDLE_FLAG_INHERIT, 0))
            {
                throw std::system_error(::GetLastError(), std::system_category(), "SetHandleInformation(socket)");
            }
#endif
            configure_socket(fd);
        }
        catch (...)
        {
            close_socket(fd);
            throw;
        }
    }
    return fd;
}

bool retry_accept(int error) noexcept
{
#if defined(_WIN32)
    return error == WSAECONNRESET || error == WSAECONNABORTED;
#else
    if (error == ECONNABORTED || error == EPROTO)
    {
        return true;
    }
#if defined(__linux__)
    // Linux reports errors from queued connections through accept itself.
    return error == ENETDOWN || error == ENOPROTOOPT || error == EHOSTDOWN || error == ENONET || error == EHOSTUNREACH
           || error == EOPNOTSUPP || error == ENETUNREACH;
#else
    return false;
#endif
#endif
}

bool accept_resource_pressure(int error) noexcept
{
#if defined(_WIN32)
    return error == WSAEMFILE || error == WSAENOBUFS;
#else
    return error == EMFILE || error == ENFILE || error == ENOBUFS || error == ENOMEM;
#endif
}

int set_option(native_socket_type fd, int level, int name, int value)
{
    return ::setsockopt(fd, level, name, reinterpret_cast<const char*>(&value), sizeof(value));
}

native_socket_type create_socket(address_family family)
{
    initialize_sockets();
#if defined(_WIN32)
    const auto fd = create_windows_socket(net::native_address_family(family), SOCK_STREAM, IPPROTO_TCP);
#elif defined(__linux__)
    const auto fd = ::socket(net::native_address_family(family), SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#else
    const auto fd = ::socket(net::native_address_family(family), SOCK_STREAM, 0);
#endif
    if (fd == invalid_socket)
    {
        socket_error("socket");
    }
    try
    {
#if !defined(__linux__)
        configure_socket(fd);
#endif
        // Outbound mapped IPv4 destinations work consistently across systems.
        // A listener overrides this before binding, defaulting to IPv6-only.
        if (family == address_family::ipv6 && set_option(fd, IPPROTO_IPV6, IPV6_V6ONLY, 0) < 0)
        {
            socket_error("setsockopt(IPV6_V6ONLY)");
        }
    }
    catch (...)
    {
        close_socket(fd);
        throw;
    }
    return fd;
}

void check_operation(io_context& context, native_socket_type fd)
{
    if (context.stopped())
    {
        throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    }
    if (fd == invalid_socket)
    {
        throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor));
    }
    if (auto* running = io_context::current(); running != nullptr && running != &context)
    {
        throw std::logic_error("socket operation used from a different io_context");
    }
}
} // namespace

stream::stream(io_context& context)
    : stream(context, address_family::unspecified)
{
}

stream::stream(io_context& context, address_family family)
    : context_(&context)
    , fd_(create_socket(family == address_family::unspecified ? address_family::ipv4 : family))
    , family_(family == address_family::unspecified ? address_family::ipv4 : family)
    , automatic_family_(family == address_family::unspecified)
{
}

stream::stream(io_context& context, native_socket_type fd, address_family family) noexcept
    : context_(&context)
    , fd_(fd)
    , family_(family)
    , automatic_family_(false)
    , connect_started_(true)
{
}

stream::~stream()
{
    close();
}

stream::stream(stream&& other) noexcept
    : read_may_be_ready_(other.read_may_be_ready_)
    , context_(other.context_)
    , fd_(std::exchange(other.fd_, invalid_socket))
    , family_(other.family_)
    , automatic_family_(other.automatic_family_)
    , connect_started_(other.connect_started_)
    , no_delay_(other.no_delay_)
{
}

stream& stream::operator=(stream&& other) noexcept
{
    if (this != &other)
    {
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

task<void> stream::connect(endpoint endpoint)
{
    check_operation(*context_, fd_);
    const auto address = net::parse_socket_address(endpoint.address, endpoint.port, endpoint.family, endpoint.scope_id);
    if (connect_started_)
    {
        throw std::logic_error("TCP connect may only be started once per stream");
    }
    if (address.family() != family_)
    {
        if (!automatic_family_)
        {
            throw std::system_error(
                std::make_error_code(std::errc::address_family_not_supported),
                "TCP connect family"
            );
        }
        const auto replacement = create_socket(address.family());
        try
        {
            if (no_delay_ && set_option(replacement, IPPROTO_TCP, TCP_NODELAY, 1) < 0)
            {
                socket_error("setsockopt(TCP_NODELAY)");
            }
        }
        catch (...)
        {
            close_socket(replacement);
            throw;
        }
        context_->release_socket(fd_);
        close_socket(std::exchange(fd_, replacement));
        family_ = address.family();
    }
    connect_started_ = true;
    int result;
    do
    {
        result = ::connect(fd_, address.data(), address.length);
    } while (result < 0 && interrupted(last_socket_error()));
    if (result == 0)
    {
        co_return;
    }
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
    {
        socket_error("getsockopt(SO_ERROR)");
    }
    if (error != 0)
    {
        throw std::system_error(socket_error_code(error), "connect");
    }
}

task<std::size_t> stream::read_some(std::span<char> buffer)
{
    check_operation(*context_, fd_);
    if (buffer.empty())
    {
        co_return 0;
    }
    if (!read_may_be_ready_)
    {
        co_await context_->wait_readable(fd_);
    }
    for (;;)
    {
        check_operation(*context_, fd_);
        if (buffer.empty())
        {
            co_return 0;
        }
        const auto count = profiling::measure(
            profiling::stage::recv,
            [&]
            { return ::recv(fd_, buffer.data(), static_cast<int>(std::min<std::size_t>(buffer.size(), INT_MAX)), 0); }
        );
        if (count >= 0)
        {
            read_may_be_ready_ = std::cmp_equal(count, std::min<std::size_t>(buffer.size(), INT_MAX));
            co_return static_cast<std::size_t>(count);
        }
        const auto error = last_socket_error();
        if (interrupted(error))
        {
            continue;
        }
        if (!would_block(error))
        {
            socket_error("recv");
        }
        co_await context_->wait_readable(fd_);
    }
}

task<void> stream::write_all(std::string_view bytes)
{
    constexpr std::size_t write_quantum = 1024 * 1024;
    auto budget = write_quantum;
    for (;;)
    {
        check_operation(*context_, fd_);
        if (bytes.empty())
        {
            co_return;
        }
#if defined(MSG_NOSIGNAL)
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const auto size = static_cast<int>(std::min(bytes.size(), budget));
        const auto count =
            profiling::measure(profiling::stage::send, [&] { return ::send(fd_, bytes.data(), size, flags); });
        if (count > 0)
        {
            const auto sent = static_cast<std::size_t>(count);
            if (sent == bytes.size())
            {
                co_return;
            }
            bytes.remove_prefix(sent);
            budget -= sent;
            if (budget == 0)
            {
                co_await context_->yield();
                if (!is_open())
                {
                    throw std::system_error(std::make_error_code(std::errc::operation_canceled));
                }
                budget = write_quantum;
            }
            continue;
        }
        if (count == 0)
        {
            throw std::system_error(std::make_error_code(std::errc::broken_pipe));
        }
        const auto error = last_socket_error();
        if (interrupted(error))
        {
            continue;
        }
        if (!would_block(error))
        {
            socket_error("send");
        }
        co_await context_->wait_writable(fd_);
        budget = write_quantum;
    }
}

void stream::set_no_delay(bool enabled)
{
    check_operation(*context_, fd_);
    const int value = enabled ? 1 : 0;
    if (set_option(fd_, IPPROTO_TCP, TCP_NODELAY, value) < 0)
    {
        socket_error("setsockopt(TCP_NODELAY)");
    }
    no_delay_ = enabled;
}

endpoint stream::local_endpoint() const
{
    check_operation(*context_, fd_);
    net::socket_address address;
    if (::getsockname(fd_, address.data(), &address.length) < 0)
    {
        socket_error("getsockname");
    }
    return net::socket_endpoint<endpoint>(address.data(), address.length);
}

endpoint stream::remote_endpoint() const
{
    check_operation(*context_, fd_);
    net::socket_address address;
    if (::getpeername(fd_, address.data(), &address.length) < 0)
    {
        socket_error("getpeername");
    }
    return net::socket_endpoint<endpoint>(address.data(), address.length);
}

task<void> stream::close_gracefully(std::chrono::milliseconds timeout, std::size_t discard_limit)
{
    try
    {
        check_operation(*context_, fd_);
#if defined(_WIN32)
        constexpr int send_side = SD_SEND;
#else
        constexpr int send_side = SHUT_WR;
#endif
        int result;
        do
        {
            result = ::shutdown(fd_, send_side);
        } while (result < 0 && interrupted(last_socket_error()));
        if (result < 0)
        {
            socket_error("shutdown(send)");
        }
        if (timeout.count() <= 0 || discard_limit == 0)
        {
            close();
            co_return;
        }

        const auto started = std::chrono::steady_clock::now();
        const auto allowed_time = std::chrono::duration<long double>(timeout);
        bool expired = false;
        auto deadline = context_->call_after(
            timeout,
            [this, &expired]
            {
                expired = true;
                close();
            }
        );
        std::array<char, 4096> discarded{};
        std::size_t total = 0;
        while (!expired && total < discard_limit
               && std::chrono::duration<long double>(std::chrono::steady_clock::now() - started) < allowed_time)
        {
            check_operation(*context_, fd_);
            const auto wanted = static_cast<int>(std::min(discarded.size(), discard_limit - total));
            const auto count =
                profiling::measure(profiling::stage::recv, [&] { return ::recv(fd_, discarded.data(), wanted, 0); });
            if (count == 0)
            {
                break;
            }
            if (count > 0)
            {
                total += static_cast<std::size_t>(count);
                continue;
            }
            const auto error = last_socket_error();
            if (interrupted(error))
            {
                continue;
            }
            if (!would_block(error))
            {
                socket_error("recv during graceful close");
            }
            try
            {
                co_await context_->wait_readable(fd_);
            }
            catch (const std::system_error&)
            {
                // Timeout closes and cancels the readiness wait. If readiness
                // was already queued, the loop's expiry check avoids using the
                // closed handle; an independent context stop still propagates.
                if (expired && !context_->stopped())
                {
                    break;
                }
                throw;
            }
        }
        if (context_->stopped())
        {
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        }
    }
    catch (...)
    {
        close();
        throw;
    }
    close();
}

void stream::close() noexcept
{
    if (fd_ != invalid_socket)
    {
        context_->release_socket(fd_);
        close_socket(std::exchange(fd_, invalid_socket));
    }
}

// NOLINTNEXTLINE(performance-unnecessary-value-param): public API; the by-value signature is kept for ABI stability
listener::listener(io_context& context, endpoint endpoint, listen_options options)
    : context_(&context)
{
    const auto address = net::parse_socket_address(endpoint.address, endpoint.port, endpoint.family, endpoint.scope_id);
    if (options.backlog <= 0)
    {
        throw std::invalid_argument("listen backlog must be positive");
    }
    if (address.family() == address_family::ipv4 && !options.ipv6_only)
    {
        throw std::invalid_argument("dual-stack listen requires an IPv6 endpoint");
    }
    family_ = address.family();
    fd_ = create_socket(family_);
    try
    {
        if (family_ == address_family::ipv6
            && set_option(fd_, IPPROTO_IPV6, IPV6_V6ONLY, options.ipv6_only ? 1 : 0) < 0)
        {
            socket_error("setsockopt(IPV6_V6ONLY)");
        }
#if defined(_WIN32)
        if (set_option(fd_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1) < 0)
        {
            socket_error("setsockopt(SO_EXCLUSIVEADDRUSE)");
        }
#else
        if (set_option(fd_, SOL_SOCKET, SO_REUSEADDR, 1) < 0)
        {
            socket_error("setsockopt(SO_REUSEADDR)");
        }
#endif
#if defined(SO_REUSEPORT_LB)
        if (options.reuse_port && set_option(fd_, SOL_SOCKET, SO_REUSEPORT_LB, 1) < 0)
        {
            socket_error("setsockopt(SO_REUSEPORT_LB)");
        }
#elif defined(SO_REUSEPORT)
        if (options.reuse_port && set_option(fd_, SOL_SOCKET, SO_REUSEPORT, 1) < 0)
        {
            socket_error("setsockopt(SO_REUSEPORT)");
        }
#else
        if (options.reuse_port)
        {
            throw std::system_error(std::make_error_code(std::errc::operation_not_supported), "SO_REUSEPORT");
        }
#endif
        if (::bind(fd_, address.data(), address.length) < 0)
        {
            socket_error("bind");
        }
        if (::listen(fd_, options.backlog) < 0)
        {
            socket_error("listen");
        }
    }
    catch (...)
    {
        close_socket(std::exchange(fd_, invalid_socket));
        throw;
    }
}

listener::~listener()
{
    close();
}

listener::listener(listener&& other) noexcept
    : context_(other.context_)
    , fd_(std::exchange(other.fd_, invalid_socket))
    , family_(other.family_)
    , accept_attempts_(other.accept_attempts_)
{
}

listener& listener::operator=(listener&& other) noexcept
{
    if (this != &other)
    {
        close();
        context_ = other.context_;
        fd_ = std::exchange(other.fd_, invalid_socket);
        family_ = other.family_;
        accept_attempts_ = other.accept_attempts_;
    }
    return *this;
}

task<stream> listener::accept()
{
    constexpr unsigned accept_quantum = 64;
    constexpr auto resource_backoff = std::chrono::milliseconds(50);
    if (accept_backoff_)
    {
        throw std::logic_error("another TCP accept is pending");
    }
    for (;;)
    {
        check_operation(*context_, fd_);
        if (accept_attempts_ == accept_quantum)
        {
            accept_attempts_ = 0;
            co_await context_->yield();
            if (fd_ == invalid_socket)
            {
                throw std::system_error(std::make_error_code(std::errc::operation_canceled));
            }
            continue;
        }
        ++accept_attempts_;
        const native_socket_type accepted =
            profiling::measure(profiling::stage::accept, [&] { return accept_socket(fd_); });
        if (accepted != invalid_socket)
        {
            co_return stream(*context_, accepted, family_);
        }
        const auto error = last_socket_error();
        if (interrupted(error) || retry_accept(error))
        {
            continue;
        }
        accept_attempts_ = 0;
        if (would_block(error))
        {
            co_await context_->wait_readable(fd_);
        }
        else if (accept_resource_pressure(error))
        {
            cancellation_source source;

            struct clear_backoff
            {
                cancellation_source*& active;

                explicit clear_backoff(cancellation_source*& slot) noexcept
                    : active(slot)
                {
                }

                clear_backoff(const clear_backoff&) = delete;
                clear_backoff& operator=(const clear_backoff&) = delete;

                ~clear_backoff()
                {
                    active = nullptr;
                }
            } reset{accept_backoff_};

            accept_backoff_ = &source;
            co_await with_cancellation(
                source.token(),
                [io = context_, resource_backoff]() -> task<void> { co_await io->sleep_for(resource_backoff); }
            );
        }
        else
        {
            throw std::system_error(socket_error_code(error), "accept");
        }
    }
}

endpoint listener::local_endpoint() const
{
    check_operation(*context_, fd_);
    net::socket_address address;
    if (::getsockname(fd_, address.data(), &address.length) < 0)
    {
        socket_error("getsockname");
    }
    return net::socket_endpoint<endpoint>(address.data(), address.length);
}

void listener::close() noexcept
{
    if (fd_ != invalid_socket)
    {
        if (accept_backoff_)
        {
            accept_backoff_->request_cancellation();
        }
        context_->release_socket(fd_);
        close_socket(std::exchange(fd_, invalid_socket));
    }
}

task<stream> connect(io_context& context, endpoint destination)
{
    stream socket(context);
    co_await socket.connect(std::move(destination));
    co_return socket;
}

task<stream> connect(endpoint destination)
{
    co_return co_await connect(this_context(), std::move(destination));
}

task<void> stream::read_exact(std::span<char> buffer)
{
    while (!buffer.empty())
    {
        const auto size = co_await read_some(buffer);
        if (!size)
        {
            throw std::system_error(
                std::make_error_code(std::errc::connection_reset),
                "EOF before read_exact completed"
            );
        }
        buffer = buffer.subspan(size);
    }
}

task<std::string> stream::read_exact(std::size_t size, std::size_t maximum)
{
    if (size > maximum)
    {
        throw std::length_error("read_exact exceeds maximum");
    }
    std::string buffer(size, '\0');
    co_await read_exact(std::span<char>(buffer));
    co_return buffer;
}
} // namespace pace::tcp
