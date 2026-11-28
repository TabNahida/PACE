#if defined(__APPLE__)
#define __APPLE_USE_RFC_3542
#endif
#include <pace/net/udp.hpp>
#include "net/adopt_socket.hpp"
#include "net/client_operation.hpp"
#include "net/socket_address.hpp"
#include "net/socket_ops.hpp"
#include "net/socket_result.hpp"
#include "net/udp_message.hpp"

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <cerrno>
#include <climits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace pace::udp
{
namespace
{
using net::close_socket;
using net::interrupted;
using net::last_socket_error;
using net::socket_error_code;
using net::socket_length;
using net::would_block;

[[noreturn]] void fail(const char* operation)
{
    net::throw_socket_error(operation);
}

void check(io_context& context, native_socket_type fd, bool asynchronous = false)
{
    if (auto error = net::socket_state(context, fd, asynchronous))
    {
        throw std::system_error(error);
    }
}

native_socket_type create_socket(address_family family)
{
    initialize_sockets();
#if defined(_WIN32)
    const auto fd = create_windows_socket(net::native_address_family(family), SOCK_DGRAM, IPPROTO_UDP);
#elif defined(__linux__)
    const auto fd =
        ::socket(net::native_address_family(family), SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
#else
    const auto fd = ::socket(net::native_address_family(family), SOCK_DGRAM, IPPROTO_UDP);
#endif
    if (fd == invalid_socket)
    {
        fail("UDP socket");
    }
    try
    {
#if !defined(__linux__)
        net::configure_socket(fd);
#endif
        if (family == address_family::ipv6)
        {
            const int yes = 1;
            if (::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&yes), sizeof(yes)) != 0)
            {
                fail("UDP IPV6_V6ONLY");
            }
        }
    }
    catch (...)
    {
        close_socket(fd);
        throw;
    }
    return fd;
}

int receive_one(native_socket_type fd, net::socket_address& sender, std::span<std::byte> buffer, bool& truncated)
{
    int count;
#if defined(_WIN32)
    count = ::recvfrom(
        fd,
        reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(buffer.size()),
        0,
        sender.data(),
        &sender.length
    );
    if (count < 0 && last_socket_error() == WSAEMSGSIZE)
    {
        count = static_cast<int>(buffer.size());
        truncated = true;
    }
#else
    iovec vector{buffer.data(), buffer.size()};
    msghdr message{};
    message.msg_name = sender.data();
    message.msg_namelen = sender.length;
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    count = static_cast<int>(::recvmsg(fd, &message, 0));
    sender.length = message.msg_namelen;
    truncated = (message.msg_flags & MSG_TRUNC) != 0;
#endif
    return count;
}
} // namespace

socket::socket(io_context& context)
    : socket(context, address_family::unspecified)
{
}

socket::socket(io_context& context, address_family family)
    : context_(&context)
    , fd_(create_socket(family == address_family::unspecified ? address_family::ipv4 : family))
    , family_(family == address_family::unspecified ? address_family::ipv4 : family)
    , automatic_family_(family == address_family::unspecified)
{
}

socket::socket(io_context& context, endpoint local, bind_options options)
    : socket(context)
{
    bind(std::move(local), options);
}

socket::socket(io_context& context, native_socket_type fd, address_family family, bool ipv6_only) noexcept
    : context_(&context)
    , fd_(fd)
    , family_(family)
    , automatic_family_(false)
    , family_selected_(true)
    , ipv6_only_(ipv6_only)
{
}

socket socket::adopt(io_context& context, native_socket_type fd)
{
    const auto family = net::inspect_socket(fd, SOCK_DGRAM);
    const bool only = family == address_family::ipv4 || net::socket_option(fd, IPPROTO_IPV6, IPV6_V6ONLY) != 0;
    const bool coalescing = receive_coalescing_enabled(fd);
    net::configure_socket(fd);
    socket result(context, fd, family, only);
    result.coalescing_ = coalescing;
    return result;
}

socket::~socket()
{
    close();
}

socket::socket(socket&& other) noexcept
    : context_(other.context_)
    , fd_(std::exchange(other.fd_, invalid_socket))
    , coalescing_(other.coalescing_)
    , family_(other.family_)
    , automatic_family_(other.automatic_family_)
    , family_selected_(other.family_selected_)
    , ipv6_only_(other.ipv6_only_)
{
}

socket& socket::operator=(socket&& other) noexcept
{
    if (this != &other)
    {
        close();
        context_ = other.context_;
        fd_ = std::exchange(other.fd_, invalid_socket);
        coalescing_ = other.coalescing_;
        family_ = other.family_;
        automatic_family_ = other.automatic_family_;
        family_selected_ = other.family_selected_;
        ipv6_only_ = other.ipv6_only_;
    }
    return *this;
}

void socket::select_family(address_family family)
{
    if (family == family_)
    {
        return;
    }
    if (!automatic_family_ || family_selected_ || sending_ || receiving_)
    {
        throw std::system_error(std::make_error_code(std::errc::address_family_not_supported), "UDP socket family");
    }
    const auto replacement = create_socket(family);
    context_->release_socket(fd_);
    close_socket(std::exchange(fd_, replacement));
    family_ = family;
}

// NOLINTNEXTLINE(performance-unnecessary-value-param): public API; the by-value signature is kept for ABI stability
void socket::bind(endpoint local, bind_options options)
{
    check(*context_, fd_);
    if (sending_ || receiving_)
    {
        throw std::logic_error("UDP bind requires an idle socket");
    }
    const auto value = net::parse_socket_address(local.address, local.port, local.family, local.scope_id);
    if (value.family() == address_family::ipv4 && !options.ipv6_only)
    {
        throw std::invalid_argument("dual-stack UDP bind requires an IPv6 endpoint");
    }
    select_family(value.family());
    if (family_ == address_family::ipv6)
    {
        const int only = options.ipv6_only ? 1 : 0;
        if (::setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&only), sizeof(only)) != 0)
        {
            fail("UDP IPV6_V6ONLY");
        }
        ipv6_only_ = options.ipv6_only;
    }
    if (options.packet_info)
    {
        enable_packet_info(fd_, family_);
    }
    if (options.transparent)
    {
        enable_transparent_udp(fd_, family_);
    }
    if (options.coalesce)
    {
        enable_coalescing(fd_);
        coalescing_ = true;
    }
    if (::bind(fd_, value.data(), value.length) != 0)
    {
        fail("UDP bind");
    }
    family_selected_ = true;
}

endpoint socket::local_endpoint() const
{
    check(*context_, fd_);
    net::socket_address local;
    if (::getsockname(fd_, local.data(), &local.length) != 0)
    {
        fail("UDP getsockname");
    }
    return net::socket_endpoint<endpoint>(local.data(), local.length);
}

void socket::set_broadcast(bool enabled)
{
    check(*context_, fd_);
    if (family_ != address_family::ipv4)
    {
        throw std::system_error(
            std::make_error_code(std::errc::operation_not_supported),
            "IPv6 does not support broadcast"
        );
    }
    const int value = enabled ? 1 : 0;
    if (::setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&value), sizeof(value)) != 0)
    {
        fail("UDP SO_BROADCAST");
    }
    family_selected_ = true;
}

task<std::size_t> socket::send_to(std::span<const std::byte> bytes, endpoint destination)
{
    return send_impl<false>(bytes, std::move(destination), nullptr);
}

task<std::size_t> socket::send_to(std::span<const std::byte> bytes, endpoint destination, packet_info info)
{
    co_return co_await send_impl<false>(bytes, std::move(destination), &info);
}

task<result<std::size_t>> socket::send_to(as_result_t, std::span<const std::byte> bytes, endpoint destination)
{
    return send_impl<true>(bytes, std::move(destination), nullptr);
}

task<result<std::size_t>> socket::send_to(
    as_result_t,
    std::span<const std::byte> bytes,
    endpoint destination,
    packet_info info
)
{
    co_return co_await send_impl<true>(bytes, std::move(destination), &info);
}

template <bool Report>
task<result_access::value<Report, std::size_t>> socket::send_impl(
    std::span<const std::byte> bytes,
    endpoint destination,
    const packet_info* info
)
{
    if (auto error = net::socket_state(*context_, fd_, true))
    {
        co_return result_access::fail<Report, std::size_t>(error, "UDP send_to");
    }

    if (bytes.size() > 65507)
    {
        co_return result_access::fail<Report, std::size_t>(std::make_error_code(std::errc::message_size), "UDP sendto");
    }
    auto target =
        net::parse_socket_address(destination.address, destination.port, destination.family, destination.scope_id);
    if (family_ == address_family::ipv6 && !ipv6_only_ && target.family() == address_family::ipv4)
    {
        target = net::map_ipv4_address(target);
    }
    select_family(target.family());
    if constexpr (Report)
    {
        if (sending_)
        {
            co_return result_access::fail<Report, std::size_t>(
                std::make_error_code(std::errc::operation_in_progress),
                "UDP send_to"
            );
        }
    }
    net::pending_operation guard(sending_, "another UDP operation is pending in this direction");
    family_selected_ = true;
    for (;;)
    {
        if (auto error = net::socket_state(*context_, fd_, true))
        {
            co_return result_access::fail<Report, std::size_t>(error, "UDP send_to");
        }
        const auto count = info ? send_message(fd_, target, bytes, *info)
                                : ::sendto(
                                      fd_,
                                      bytes.empty() ? "" : reinterpret_cast<const char*>(bytes.data()),
                                      static_cast<int>(bytes.size()),
                                      0,
                                      target.data(),
                                      target.length
                                  );
        if (count >= 0)
        {
            if (static_cast<std::size_t>(count) != bytes.size())
            {
                co_return result_access::fail<Report, std::size_t>(
                    std::make_error_code(std::errc::message_size),
                    "UDP sendto"
                );
            }
            co_return static_cast<std::size_t>(count);
        }
        const int error = last_socket_error();
        if (interrupted(error))
        {
            continue;
        }
        if (!would_block(error))
        {
            co_return result_access::fail<Report, std::size_t>(socket_error_code(error), "UDP sendto");
        }
        if (auto reason = co_await net::socket_wait{context_->wait_writable(fd_)})
        {
            co_return result_access::fail<Report, std::size_t>(reason, "UDP send_to");
        }
    }
}

task<datagram> socket::receive_from(std::span<std::byte> buffer)
{
    return receive_impl<false>(buffer, nullptr);
}

task<datagram> socket::receive_from(std::span<std::byte> buffer, packet_info& info)
{
    return receive_impl<false>(buffer, &info);
}

task<result<datagram>> socket::receive_from(as_result_t, std::span<std::byte> buffer)
{
    return receive_impl<true>(buffer, nullptr);
}

task<result<datagram>> socket::receive_from(as_result_t, std::span<std::byte> buffer, packet_info& info)
{
    return receive_impl<true>(buffer, &info);
}

template <bool Report>
task<result_access::value<Report, datagram>> socket::receive_impl(std::span<std::byte> buffer, packet_info* info)
{
    if (auto error = net::socket_state(*context_, fd_, true))
    {
        co_return result_access::fail<Report, datagram>(error, "UDP receive_from");
    }

    if (coalescing_ && !info)
    {
        throw std::logic_error("UDP coalescing requires receive_from with packet_info");
    }
    if constexpr (Report)
    {
        if (receiving_)
        {
            co_return result_access::fail<Report, datagram>(
                std::make_error_code(std::errc::operation_in_progress),
                "UDP receive_from"
            );
        }
    }
    net::pending_operation guard(receiving_, "another UDP operation is pending in this direction");
    if (buffer.empty())
    {
        throw std::invalid_argument("UDP receive buffer must not be empty");
    }
    family_selected_ = true;
    for (;;)
    {
        if (auto error = net::socket_state(*context_, fd_, true))
        {
            co_return result_access::fail<Report, datagram>(error, "UDP receive_from");
        }
        net::socket_address sender;
        bool truncated = false;
        const auto capacity = std::min<std::size_t>(buffer.size(), INT_MAX);
        int count;
        if (info)
        {
            count = receive_message(fd_, sender, buffer.first(capacity), truncated, *info);
        }
        else
        {
            count = receive_one(fd_, sender, buffer.first(capacity), truncated);
        }
        if (count >= 0)
        {
            co_return datagram{
                std::min(static_cast<std::size_t>(count), capacity),
                net::socket_endpoint<endpoint>(sender.data(), sender.length),
                truncated
            };
        }
        const int error = last_socket_error();
        if (interrupted(error))
        {
            continue;
        }
        if (!would_block(error))
        {
            co_return result_access::fail<Report, datagram>(socket_error_code(error), "UDP receive");
        }
        if (auto reason = co_await net::socket_wait{context_->wait_readable(fd_)})
        {
            co_return result_access::fail<Report, datagram>(reason, "UDP receive_from");
        }
    }
}

task<std::size_t> socket::send_batch(std::span<const outgoing_datagram> messages)
{
    check(*context_, fd_, true);
    if (sending_)
    {
        throw std::logic_error("another UDP operation is pending in this direction");
    }
    if (messages.empty())
    {
        co_return 0;
    }
    constexpr std::size_t limit = 32;
    messages = messages.first(std::min(limit, messages.size()));
    std::array<net::socket_address, limit> targets{};
#if defined(__linux__)
    std::array<mmsghdr, limit> native{};
    std::array<iovec, limit> vectors{};
#endif
    for (std::size_t i = 0; i < messages.size(); ++i)
    {
        const auto& [bytes, peer] = messages[i];
        if (bytes.size() > 65507)
        {
            throw std::system_error(std::make_error_code(std::errc::message_size));
        }
        auto& target = targets[i];
        target = net::parse_socket_address(peer.address, peer.port, peer.family, peer.scope_id);
        if (family_ == address_family::ipv6 && !ipv6_only_ && target.family() == address_family::ipv4)
        {
            target = net::map_ipv4_address(target);
        }
        if (target.family() != targets[0].family())
        {
            throw std::invalid_argument("UDP send_batch requires one address family");
        }
#if defined(__linux__)
        vectors[i] = {const_cast<std::byte*>(bytes.data()), bytes.size()};
        auto& header = native[i].msg_hdr;
        header.msg_name = target.data();
        header.msg_namelen = target.length;
        header.msg_iov = &vectors[i];
        header.msg_iovlen = 1;
#endif
    }
    select_family(targets[0].family());
    net::pending_operation guard(sending_, "another UDP operation is pending in this direction");
    family_selected_ = true;
    for (;;)
    {
        check(*context_, fd_, true);
#if defined(__linux__)
        const auto count = ::sendmmsg(fd_, native.data(), static_cast<unsigned>(messages.size()), 0);
        if (count > 0)
        {
            co_return static_cast<std::size_t>(count);
        }
#else
        std::size_t count = 0;
        for (; count < messages.size(); ++count)
        {
            const auto bytes = messages[count].bytes;
            if (::sendto(
                    fd_,
                    bytes.empty() ? "" : reinterpret_cast<const char*>(bytes.data()),
                    static_cast<int>(bytes.size()),
                    0,
                    targets[count].data(),
                    targets[count].length
                )
                < 0)
            {
                break;
            }
        }
        if (count)
        {
            co_return count;
        }
#endif
        const auto error = last_socket_error();
        if (interrupted(error))
        {
            continue;
        }
        if (!would_block(error))
        {
            throw std::system_error(socket_error_code(error), "UDP send_batch");
        }
        co_await context_->wait_writable(fd_);
    }
}

task<std::size_t> socket::receive_batch(std::span<incoming_datagram> messages)
{
    check(*context_, fd_, true);
    net::pending_operation guard(receiving_, "another UDP operation is pending in this direction");
    if (coalescing_)
    {
        throw std::logic_error("UDP coalescing requires receive_from with packet_info");
    }
    if (messages.empty())
    {
        co_return 0;
    }
    constexpr std::size_t limit = 32;
    messages = messages.first(std::min(limit, messages.size()));
    std::array<net::socket_address, limit> senders{};
#if defined(__linux__)
    std::array<mmsghdr, limit> native{};
    std::array<iovec, limit> vectors{};
#endif
    for (std::size_t i = 0; i < messages.size(); ++i)
    {
        const auto buffer = messages[i].buffer;
        if (buffer.empty())
        {
            throw std::invalid_argument("UDP receive_batch buffers must not be empty");
        }
#if defined(__linux__)
        vectors[i] = {buffer.data(), std::min<std::size_t>(buffer.size(), INT_MAX)};
        auto& header = native[i].msg_hdr;
        header.msg_name = senders[i].data();
        header.msg_namelen = senders[i].length;
        header.msg_iov = &vectors[i];
        header.msg_iovlen = 1;
#endif
    }
    family_selected_ = true;
    for (;;)
    {
        check(*context_, fd_, true);
#if defined(__linux__)
        const int count = ::recvmmsg(fd_, native.data(), static_cast<unsigned>(messages.size()), MSG_DONTWAIT, nullptr);
        for (int i = 0; i < count; ++i)
        {
            const auto& header = native[i].msg_hdr;
            messages[i].result = {
                native[i].msg_len,
                net::socket_endpoint<endpoint>(senders[i].data(), header.msg_namelen),
                (header.msg_flags & MSG_TRUNC) != 0
            };
        }
#else
        std::size_t count = 0;
        for (; count < messages.size(); ++count)
        {
            bool truncated = false;
            auto buffer = messages[count].buffer;
            auto& sender = senders[count];
            const int size =
                receive_one(fd_, sender, buffer.first(std::min<std::size_t>(buffer.size(), INT_MAX)), truncated);
            if (size < 0)
            {
                break;
            }
            messages[count].result = {
                static_cast<std::size_t>(size),
                net::socket_endpoint<endpoint>(sender.data(), sender.length),
                truncated
            };
        }
#endif
        if (count > 0)
        {
            co_return static_cast<std::size_t>(count);
        }
        const auto error = last_socket_error();
        if (interrupted(error))
        {
            continue;
        }
        if (!would_block(error))
        {
            throw std::system_error(socket_error_code(error), "UDP receive_batch");
        }
        co_await context_->wait_readable(fd_);
    }
}

void socket::close() noexcept
{
    if (fd_ != invalid_socket)
    {
        context_->release_socket(fd_);
        close_socket(std::exchange(fd_, invalid_socket));
    }
}

static task<packet> exchange_datagram(endpoint peer, std::string request, std::size_t capacity)
{
    if (capacity == 0)
    {
        throw std::invalid_argument("UDP receive capacity must be positive");
    }
    if (capacity > 65536)
    {
        throw std::length_error("UDP receive capacity exceeds 64 KiB");
    }
    if (peer.port == 0)
    {
        throw std::invalid_argument("UDP peer port must be nonzero");
    }
    const auto native = net::parse_socket_address(peer.address, peer.port, peer.family, peer.scope_id);
    const auto expected = net::socket_endpoint<endpoint>(native.data(), native.length);
    auto& io = this_context();
    socket connection(io, native.family());
    co_await connection.send(std::move(request), std::move(peer));
    std::string data(capacity, '\0');
    unsigned skipped = 0;
    for (;;)
    {
        auto reply = co_await connection.receive_from(as_writable_bytes(data));
        if (reply.sender.address == expected.address && reply.sender.port == expected.port
            && reply.sender.family == expected.family && reply.sender.scope_id == expected.scope_id)
        {
            data.resize(reply.size);
            co_return packet{std::move(data), std::move(reply.sender), reply.truncated};
        }
        if (++skipped == 16)
        {
            skipped = 0;
            co_await io.yield();
        }
    }
}

task<packet> async_exchange(endpoint peer, std::string request, client_options options)
{
    auto work = exchange_datagram(std::move(peer), std::move(request), options.max_reply_size.value_or(65536));
    co_return co_await net::client_operation(options, std::move(work));
}

packet exchange(endpoint peer, std::string request, client_options options)
{
    auto budget = net::take_blocking_budget(options);
    return blocking_call(
        [&] { return async_exchange(std::move(peer), std::move(request), std::move(options)); },
        std::move(budget)
    );
}

std::size_t send(endpoint peer, std::string message, client_options options)
{
    auto budget = net::take_blocking_budget(options);
    return blocking_call(
        [&]() -> task<std::size_t>
        {
            auto socket = open();
            co_return co_await socket.send(std::move(message), std::move(peer));
        },
        std::move(budget)
    );
}
} // namespace pace::udp
