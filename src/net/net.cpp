#include <pace/net/tcp.hpp>
#include <pace/runtime/operation.hpp>
#include <pace/runtime/this_context.hpp>
#include "net/adopt_socket.hpp"
#include "net/client_operation.hpp"
#include "net/socket_address.hpp"
#include "net/socket_ops.hpp"
#include "net/socket_result.hpp"
#include "net/tcp_options.hpp"
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
#if defined(__APPLE__)
#include <netinet/tcp_fsm.h>
#endif
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
            net::configure_stream(fd);
        }
        catch (...)
        {
            close_socket(fd);
            throw;
        }
    }
    return fd;
}

bool is_listener(native_socket_type fd)
{
#if defined(__APPLE__)
    tcp_connection_info info{};
    socket_length size = sizeof(info);
    if (::getsockopt(fd, IPPROTO_TCP, TCP_CONNECTION_INFO, &info, &size) != 0)
    {
        socket_error("getsockopt(TCP_CONNECTION_INFO)");
    }
    return info.tcpi_state == TCPS_LISTEN;
#else
    return net::socket_option(fd, SOL_SOCKET, SO_ACCEPTCONN) != 0;
#endif
}

bool retry_accept(int error) noexcept
{
#if defined(_WIN32)
    return error == WSAECONNRESET || error == WSAECONNABORTED;
#else
    const bool aborted = error == ECONNABORTED || error == EPROTO;
#if defined(__linux__)
    // Linux reports errors from queued connections through accept itself.
    return aborted || error == ENETDOWN || error == ENOPROTOOPT || error == EHOSTDOWN || error == ENONET
           || error == EHOSTUNREACH || error == EOPNOTSUPP || error == ENETUNREACH;
#else
    return aborted;
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
        net::configure_stream(fd);
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
    if (auto error = net::socket_state(context, fd))
    {
        throw std::system_error(error);
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

stream stream::adopt(io_context& context, native_socket_type fd)
{
    const auto family = net::inspect_socket(fd, SOCK_STREAM);
    if (is_listener(fd))
    {
        throw std::invalid_argument("cannot adopt a listener as a TCP stream");
    }
    net::socket_address peer;
    const bool connected = ::getpeername(fd, peer.data(), &peer.length) == 0;
    if (!connected)
    {
        const int error = last_socket_error();
#if defined(_WIN32)
        if (error != WSAENOTCONN && error != WSAEINVAL)
#else
        if (error != ENOTCONN)
#endif
        {
            throw std::system_error(socket_error_code(error), "getpeername");
        }
    }
    net::configure_stream(fd);
    stream result(context, fd, family);
    result.connect_started_ = connected;
    return result;
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

task<void> stream::connect(endpoint destination, connect_options options)
{
    return connect_impl<false>(std::move(destination), std::move(options));
}

task<result<void>> stream::connect(as_result_t, endpoint destination, connect_options options)
{
    return connect_impl<true>(std::move(destination), std::move(options));
}

template <bool Report>
task<result_access::value<Report, void>> stream::connect_impl(endpoint destination, connect_options options)
{
    if (auto error = net::socket_state(*context_, fd_))
    {
        co_return result_access::fail<Report, void>(error, "TCP connect");
    }

    const auto address =
        net::parse_socket_address(destination.address, destination.port, destination.family, destination.scope_id);
    if (connect_started_)
    {
        throw std::logic_error("TCP connect may only be started once per stream");
    }
    if (address.family() != family_)
    {
        if (!automatic_family_)
        {
            co_return result_access::fail<Report, void>(
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
    net::apply_tcp_options(fd_, family_, options.socket, 0);
    if (options.pre_connect)
    {
        options.pre_connect(fd_);
        if (auto error = net::socket_state(*context_, fd_))
        {
            co_return result_access::fail<Report, void>(error, "TCP connect");
        }
    }
    int result;
    do
    {
#if defined(__APPLE__)
        if (options.socket.fast_open)
        {
            sa_endpoints_t endpoints{};
            endpoints.sae_dstaddr = address.data();
            endpoints.sae_dstaddrlen = address.length;
            result = ::connectx(
                fd_,
                &endpoints,
                SAE_ASSOCID_ANY,
                CONNECT_RESUME_ON_READ_WRITE | CONNECT_DATA_IDEMPOTENT,
                nullptr,
                0,
                nullptr,
                nullptr
            );
        }
        else
#endif
        {
            result = ::connect(fd_, address.data(), address.length);
        }
    } while (result < 0 && interrupted(last_socket_error()));
    if (result == 0)
    {
        co_return result_access::success<Report>();
    }
    const auto pending_error = last_socket_error();
#if defined(_WIN32)
    const bool failed =
        pending_error != WSAEWOULDBLOCK && pending_error != WSAEINPROGRESS && pending_error != WSAEALREADY;
#else
    const bool failed = pending_error != EINPROGRESS && pending_error != EALREADY;
#endif
    if (failed)
    {
        co_return result_access::fail<Report, void>(socket_error_code(pending_error), "connect");
    }
    if (auto reason = co_await net::socket_wait{context_->wait_writable(fd_)})
    {
        co_return result_access::fail<Report, void>(reason, "TCP connect");
    }
    if (auto error = net::socket_state(*context_, fd_))
    {
        co_return result_access::fail<Report, void>(error, "TCP connect");
    }

    int error = 0;
    socket_length length = sizeof(error);
    if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) < 0)
    {
        co_return result_access::fail<Report, void>(socket_error_code(last_socket_error()), "getsockopt(SO_ERROR)");
    }
    if (error != 0)
    {
        co_return result_access::fail<Report, void>(socket_error_code(error), "connect");
    }
    co_return result_access::success<Report>();
}

task<std::size_t> stream::read_some(std::span<std::byte> buffer)
{
    return read_impl<false>(buffer);
}

task<result<std::size_t>> stream::read_some(as_result_t, std::span<std::byte> buffer)
{
    return read_impl<true>(buffer);
}

template <bool Report>
task<result_access::value<Report, std::size_t>> stream::read_impl(std::span<std::byte> buffer)
{
    if (auto error = net::socket_state(*context_, fd_))
    {
        co_return result_access::fail<Report, std::size_t>(error, "TCP read_some");
    }

    if constexpr (Report)
    {
        if (reading_)
        {
            co_return result_access::fail<Report, std::size_t>(
                std::make_error_code(std::errc::operation_in_progress),
                "TCP read"
            );
        }
    }
    net::pending_operation guard(reading_, "another TCP read is pending");
    if (buffer.empty())
    {
        co_return 0;
    }
    if (!read_may_be_ready_)
    {
        if (auto reason = co_await net::socket_wait{context_->wait_readable(fd_)})
        {
            co_return result_access::fail<Report, std::size_t>(reason, "TCP read_some");
        }
    }
    for (;;)
    {
        if (auto error = net::socket_state(*context_, fd_))
        {
            co_return result_access::fail<Report, std::size_t>(error, "TCP read_some");
        }

        const auto count = profiling::measure(
            profiling::stage::recv,
            [&]
            {
                return ::recv(
                    fd_,
                    reinterpret_cast<char*>(buffer.data()),
                    static_cast<int>(std::min<std::size_t>(buffer.size(), INT_MAX)),
                    0
                );
            }
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
            co_return result_access::fail<Report, std::size_t>(socket_error_code(error), "recv");
        }
        if (auto reason = co_await net::socket_wait{context_->wait_readable(fd_)})
        {
            co_return result_access::fail<Report, std::size_t>(reason, "TCP read_some");
        }
    }
}

task<void> stream::write_all(std::span<const std::byte> bytes)
{
    return write_impl<false>(bytes);
}

task<result<void>> stream::write_all(as_result_t, std::span<const std::byte> bytes)
{
    return write_impl<true>(bytes);
}

template <bool Report>
task<result_access::value<Report, void>> stream::write_impl(std::span<const std::byte> bytes)
{
    constexpr std::size_t write_quantum = std::size_t{1024} * 1024;
    auto budget = write_quantum;
    if constexpr (Report)
    {
        if (writing_)
        {
            co_return result_access::fail<Report, void>(
                std::make_error_code(std::errc::operation_in_progress),
                "TCP write"
            );
        }
    }
    net::pending_operation guard(writing_, "another TCP write is pending");
    for (;;)
    {
        if (auto error = net::socket_state(*context_, fd_))
        {
            co_return result_access::fail<Report, void>(error, "TCP write_all");
        }

        if (bytes.empty())
        {
            co_return result_access::success<Report>();
        }
#if defined(MSG_NOSIGNAL)
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const auto size = static_cast<int>(std::min(bytes.size(), budget));
        const auto count = profiling::measure(
            profiling::stage::send,
            [&] { return ::send(fd_, reinterpret_cast<const char*>(bytes.data()), size, flags); }
        );
        if (count > 0)
        {
            const auto sent = static_cast<std::size_t>(count);
            if (sent == bytes.size())
            {
                co_return result_access::success<Report>();
            }
            bytes = bytes.subspan(sent);
            budget -= sent;
            if (budget == 0)
            {
                co_await context_->yield();
                if (!is_open())
                {
                    co_return result_access::fail<Report, void>(
                        std::make_error_code(std::errc::operation_canceled),
                        "TCP write_all"
                    );
                }
                budget = write_quantum;
            }
            continue;
        }
        if (count == 0)
        {
            co_return result_access::fail<Report, void>(std::make_error_code(std::errc::broken_pipe), "TCP write_all");
        }
        const auto error = last_socket_error();
        if (interrupted(error))
        {
            continue;
        }
        if (!would_block(error))
        {
            co_return result_access::fail<Report, void>(socket_error_code(error), "send");
        }
        if (auto reason = co_await net::socket_wait{context_->wait_writable(fd_)})
        {
            co_return result_access::fail<Report, void>(reason, "TCP write_all");
        }
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
        net::pending_operation guard(reading_, "another TCP read is pending");
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
        net::apply_tcp_options(fd_, family_, options.socket, options.backlog);
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

listener::listener(io_context& context, native_socket_type fd, address_family family) noexcept
    : context_(&context)
    , fd_(fd)
    , family_(family)
{
}

listener listener::adopt(io_context& context, native_socket_type fd)
{
    const auto family = net::inspect_socket(fd, SOCK_STREAM);
    if (!is_listener(fd))
    {
        throw std::invalid_argument("adopt requires a listening TCP socket");
    }
    net::configure_stream(fd);
    return listener(context, fd, family);
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
    return accept_impl<false>();
}

task<result<stream>> listener::accept(as_result_t)
{
    return accept_impl<true>();
}

template <bool Report>
task<result_access::value<Report, stream>> listener::accept_impl()
{
    constexpr unsigned accept_quantum = 64;
    constexpr auto resource_backoff = std::chrono::milliseconds(50);
    if constexpr (Report)
    {
        if (accepting_)
        {
            co_return result_access::fail<Report, stream>(
                std::make_error_code(std::errc::operation_in_progress),
                "TCP accept"
            );
        }
    }
    net::pending_operation guard(accepting_, "another TCP accept is pending");
    for (;;)
    {
        if (auto error = net::socket_state(*context_, fd_))
        {
            co_return result_access::fail<Report, stream>(error, "TCP accept");
        }

        if (accept_attempts_ == accept_quantum)
        {
            accept_attempts_ = 0;
            co_await context_->yield();
            if (fd_ == invalid_socket)
            {
                co_return result_access::fail<Report, stream>(
                    std::make_error_code(std::errc::operation_canceled),
                    "TCP accept"
                );
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
            if (auto reason = co_await net::socket_wait{context_->wait_readable(fd_)})
            {
                co_return result_access::fail<Report, stream>(reason, "TCP accept");
            }
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
            co_return result_access::fail<Report, stream>(socket_error_code(error), "accept");
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

namespace
{
template <bool Report>
task<result_access::value<Report, stream>> connect_stream(endpoint destination, connect_options transport)
{
    stream socket(this_context());
    if constexpr (Report)
    {
        auto connected = co_await socket.connect(as_result, std::move(destination), std::move(transport));
        if (!connected)
        {
            co_return result<stream>::failure(connected.error());
        }
    }
    else
    {
        co_await socket.connect(std::move(destination), std::move(transport));
    }
    co_return std::move(socket);
}
} // namespace

task<stream> connect(endpoint destination, client_options options)
{
    co_return co_await net::client_operation(
        options,
        connect_stream<false>(std::move(destination), std::move(options.transport))
    );
}

task<result<stream>> connect(as_result_t, endpoint destination, client_options options)
{
    co_return co_await net::client_operation(
        options,
        connect_stream<true>(std::move(destination), std::move(options.transport))
    );
}

task<std::string> async_exchange(endpoint peer, std::string request, std::size_t reply_size, client_options options)
{
    auto connection = connect_stream<false>(std::move(peer), std::move(options.transport));
    auto work = net::exchange_bytes(
        std::move(connection),
        std::move(request),
        reply_size,
        options.max_reply_size.value_or(std::size_t{1024} * 1024)
    );
    co_return co_await net::client_operation(options, std::move(work));
}

std::string exchange(endpoint peer, std::string request, std::size_t reply_size, client_options options)
{
    auto budget = net::take_blocking_budget(options);
    return blocking_call(
        [&] { return async_exchange(std::move(peer), std::move(request), reply_size, std::move(options)); },
        std::move(budget)
    );
}

task<void> stream::read_exact(std::span<std::byte> buffer)
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
    co_await read_exact(as_writable_bytes(buffer));
    co_return buffer;
}

task<void> stream::shutdown_write()
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
    co_return;
}

static_assert(byte_stream<stream>);
} // namespace pace::tcp
