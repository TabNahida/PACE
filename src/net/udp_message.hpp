#pragma once

#include <pace/net/udp.hpp>
#include "net/socket_address.hpp"
#include "net/socket_ops.hpp"

#if defined(_WIN32)
#include <mswsock.h>
#else
#include <sys/uio.h>
#if defined(__linux__)
#include <linux/udp.h>
#endif
#endif

#include <array>
#include <climits>
#include <cstring>

namespace pace::udp
{
namespace
{
#if defined(_WIN32)
using control_header = WSACMSGHDR;
using native_message = WSAMSG;
using ipv4_packet_info = IN_PKTINFO;
using ipv6_packet_info = IN6_PKTINFO;
#define PACE_CMSG_FIRST WSA_CMSG_FIRSTHDR
#define PACE_CMSG_NEXT WSA_CMSG_NXTHDR
#define PACE_CMSG_DATA WSA_CMSG_DATA
#define PACE_CMSG_LEN WSA_CMSG_LEN
#define PACE_CMSG_SPACE WSA_CMSG_SPACE
#else
using control_header = cmsghdr;
using native_message = msghdr;
#if defined(IP_PKTINFO)
using ipv4_packet_info = in_pktinfo;
#endif
using ipv6_packet_info = in6_pktinfo;
#define PACE_CMSG_FIRST CMSG_FIRSTHDR
#define PACE_CMSG_NEXT CMSG_NXTHDR
#define PACE_CMSG_DATA CMSG_DATA
#define PACE_CMSG_LEN CMSG_LEN
#define PACE_CMSG_SPACE CMSG_SPACE
#endif

struct control_buffer
{
    alignas(control_header) std::array<std::byte, 256> bytes{};
    std::size_t used = 0;

    template <class T>
    void append(int level, int type, const T& value)
    {
        const auto space = PACE_CMSG_SPACE(sizeof(T));
        if (space > bytes.size() - used)
        {
            throw std::length_error("UDP control buffer full");
        }
        auto* header = reinterpret_cast<control_header*>(bytes.data() + used);
        header->cmsg_level = level;
        header->cmsg_type = type;
        header->cmsg_len = PACE_CMSG_LEN(sizeof(T));
        std::memcpy(PACE_CMSG_DATA(header), &value, sizeof(T));
        used += space;
    }
};

template <class T>
T control_value(control_header* header)
{
    if (header->cmsg_len < PACE_CMSG_LEN(sizeof(T)))
    {
        throw std::system_error(std::make_error_code(std::errc::message_size), "short UDP control message");
    }
    T value{};
    std::memcpy(&value, PACE_CMSG_DATA(header), sizeof(T));
    return value;
}

void enable_control(native_socket_type fd, int level, int name)
{
    const int yes = 1;
    if (::setsockopt(fd, level, name, reinterpret_cast<const char*>(&yes), sizeof(yes)) != 0)
    {
        net::throw_socket_error("enable UDP packet information");
    }
}

void enable_packet_info(native_socket_type fd, address_family family)
{
    if (family == address_family::ipv6)
    {
#if defined(_WIN32)
        enable_control(fd, IPPROTO_IPV6, IPV6_PKTINFO);
#else
        enable_control(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO);
#endif
#if defined(_WIN32) && defined(IPV6_ECN)
        enable_control(fd, IPPROTO_IPV6, IPV6_ECN);
#elif !defined(_WIN32)
        enable_control(fd, IPPROTO_IPV6, IPV6_RECVTCLASS);
#endif
    }
    else
    {
#if defined(IP_PKTINFO)
        enable_control(fd, IPPROTO_IP, IP_PKTINFO);
#else
        enable_control(fd, IPPROTO_IP, IP_RECVDSTADDR);
#endif
#if defined(_WIN32) && defined(IP_ECN)
        enable_control(fd, IPPROTO_IP, IP_ECN);
#elif !defined(_WIN32)
        enable_control(fd, IPPROTO_IP, IP_RECVTOS);
#endif
    }
}

void enable_transparent_udp(native_socket_type fd, address_family family)
{
#if defined(IP_TRANSPARENT) && defined(IP_RECVORIGDSTADDR) && defined(IPV6_TRANSPARENT) && defined(IPV6_RECVORIGDSTADDR)
    if (family == address_family::ipv4)
    {
        enable_control(fd, IPPROTO_IP, IP_TRANSPARENT);
        enable_control(fd, IPPROTO_IP, IP_RECVORIGDSTADDR);
    }
    else
    {
        enable_control(fd, IPPROTO_IPV6, IPV6_TRANSPARENT);
        enable_control(fd, IPPROTO_IPV6, IPV6_RECVORIGDSTADDR);
    }
#else
    (void)fd;
    (void)family;
    throw std::system_error(std::make_error_code(std::errc::operation_not_supported), "UDP transparent bind");
#endif
}

void enable_coalescing(native_socket_type fd)
{
#if defined(UDP_GRO)
    enable_control(fd, IPPROTO_UDP, UDP_GRO);
#elif defined(UDP_RECV_MAX_COALESCED_SIZE)
    const DWORD size = 65535;
    if (::setsockopt(fd, IPPROTO_UDP, UDP_RECV_MAX_COALESCED_SIZE, reinterpret_cast<const char*>(&size), sizeof(size))
        != 0)
    {
        net::throw_socket_error("UDP_RECV_MAX_COALESCED_SIZE");
    }
#else
    (void)fd;
    throw std::system_error(std::make_error_code(std::errc::operation_not_supported), "UDP receive coalescing");
#endif
}

bool receive_coalescing_enabled(native_socket_type fd)
{
#if defined(UDP_GRO) || defined(UDP_RECV_MAX_COALESCED_SIZE)
#if defined(UDP_GRO)
    constexpr int option = UDP_GRO;
#else
    constexpr int option = UDP_RECV_MAX_COALESCED_SIZE;
#endif
    int value = 0;
    net::socket_length size = sizeof(value);
    if (::getsockopt(fd, IPPROTO_UDP, option, reinterpret_cast<char*>(&value), &size) != 0)
    {
        const int error = net::last_socket_error();
#if defined(_WIN32)
        if (error == WSAENOPROTOOPT || error == WSAEOPNOTSUPP)
#else
        if (error == ENOPROTOOPT || error == EOPNOTSUPP)
#endif
        {
            return false;
        }
        throw std::system_error(net::socket_error_code(error), "getsockopt(UDP receive coalescing)");
    }
    return value != 0;
#else
    (void)fd;
    return false;
#endif
}

std::string numeric_address(int family, const void* address)
{
    char text[INET6_ADDRSTRLEN]{};
    if (!::inet_ntop(family, address, text, sizeof(text)))
    {
        net::throw_socket_error("inet_ntop");
    }
    return text;
}

packet_info decode_control(native_message& message)
{
    packet_info result;
    for (auto* h = PACE_CMSG_FIRST(&message); h; h = PACE_CMSG_NEXT(&message, h))
    {
#if defined(UDP_GRO)
        if (h->cmsg_level == IPPROTO_UDP && h->cmsg_type == UDP_GRO)
        {
            result.segment_size = static_cast<std::uint16_t>(control_value<int>(h));
        }
#elif defined(UDP_COALESCED_INFO)
        if (h->cmsg_level == IPPROTO_UDP && h->cmsg_type == UDP_COALESCED_INFO)
        {
            result.segment_size = static_cast<std::uint16_t>(control_value<DWORD>(h));
        }
#endif
#if defined(__linux__) && defined(IP_ORIGDSTADDR) && defined(IPV6_ORIGDSTADDR)
        if (h->cmsg_level == IPPROTO_IP && h->cmsg_type == IP_ORIGDSTADDR)
        {
            const auto original = control_value<sockaddr_in>(h);
            result.original_destination =
                net::socket_endpoint<endpoint>(reinterpret_cast<const sockaddr*>(&original), sizeof(original));
        }
        if (h->cmsg_level == IPPROTO_IPV6 && h->cmsg_type == IPV6_ORIGDSTADDR)
        {
            const auto original = control_value<sockaddr_in6>(h);
            result.original_destination =
                net::socket_endpoint<endpoint>(reinterpret_cast<const sockaddr*>(&original), sizeof(original));
        }
#endif
        if (h->cmsg_level == IPPROTO_IP)
        {
#if defined(IP_PKTINFO)
            if (h->cmsg_type == IP_PKTINFO)
            {
                const auto info = control_value<ipv4_packet_info>(h);
                result.local_address = numeric_address(AF_INET, &info.ipi_addr);
                result.interface_index = info.ipi_ifindex;
            }
#else
            if (h->cmsg_type == IP_RECVDSTADDR)
            {
                const auto address = control_value<in_addr>(h);
                result.local_address = numeric_address(AF_INET, &address);
            }
#endif
#if defined(_WIN32) && defined(IP_ECN)
            if (h->cmsg_type == IP_ECN)
            {
                result.ecn = control_value<int>(h) & 3;
            }
#elif !defined(_WIN32)
            if (h->cmsg_type == IP_TOS || h->cmsg_type == IP_RECVTOS)
            {
                result.ecn = control_value<unsigned char>(h) & 3;
            }
#endif
        }
        if (h->cmsg_level == IPPROTO_IPV6)
        {
            if (h->cmsg_type == IPV6_PKTINFO)
            {
                const auto info = control_value<ipv6_packet_info>(h);
                result.local_address = numeric_address(AF_INET6, &info.ipi6_addr);
                result.interface_index = info.ipi6_ifindex;
            }
#if defined(_WIN32) && defined(IPV6_ECN)
            if (h->cmsg_type == IPV6_ECN)
#elif !defined(_WIN32)
            if (h->cmsg_type == IPV6_TCLASS)
#endif
#if !defined(_WIN32) || defined(IPV6_ECN)
            {
                result.ecn = control_value<int>(h) & 3;
            }
#endif
        }
    }
    return result;
}

void encode_control(control_buffer& control, const packet_info& info, address_family family)
{
    const bool v4 = family == address_family::ipv4;
    if (!info.local_address.empty() || info.interface_index)
    {
        if (info.local_address.find('\0') != std::string::npos)
        {
            throw std::invalid_argument("invalid UDP source address");
        }
        if (v4)
        {
            in_addr source{};
            if (!info.local_address.empty() && ::inet_pton(AF_INET, info.local_address.c_str(), &source) != 1)
            {
                throw std::invalid_argument("invalid IPv4 UDP source address");
            }
#if defined(IP_PKTINFO)
            ipv4_packet_info value{};
#if defined(_WIN32)
            value.ipi_addr = source;
#else
            value.ipi_spec_dst = source;
#endif
            value.ipi_ifindex = info.interface_index;
            control.append(IPPROTO_IP, IP_PKTINFO, value);
#else
            if (info.interface_index)
            {
                throw std::system_error(
                    std::make_error_code(std::errc::operation_not_supported),
                    "IPv4 send interface"
                );
            }
            control.append(IPPROTO_IP, IP_SENDSRCADDR, source);
#endif
        }
        else
        {
            ipv6_packet_info value{};
            if (!info.local_address.empty() && ::inet_pton(AF_INET6, info.local_address.c_str(), &value.ipi6_addr) != 1)
            {
                throw std::invalid_argument("invalid IPv6 UDP source address");
            }
            value.ipi6_ifindex = info.interface_index;
            control.append(IPPROTO_IPV6, IPV6_PKTINFO, value);
        }
    }
    if (info.segment_size)
    {
#if defined(UDP_SEGMENT)
        control.append(IPPROTO_UDP, UDP_SEGMENT, info.segment_size);
#elif defined(UDP_SEND_MSG_SIZE)
        control.append(IPPROTO_UDP, UDP_SEND_MSG_SIZE, static_cast<DWORD>(info.segment_size));
#else
        throw std::system_error(std::make_error_code(std::errc::operation_not_supported), "UDP segmentation");
#endif
    }
    if (info.ecn)
    {
        if (*info.ecn > 3)
        {
            throw std::invalid_argument("UDP ECN must be 0..3");
        }
#if defined(_WIN32) && defined(IP_ECN) && defined(IPV6_ECN)
        control.append(v4 ? IPPROTO_IP : IPPROTO_IPV6, v4 ? IP_ECN : IPV6_ECN, int(*info.ecn));
#elif defined(__FreeBSD__)
        if (v4)
        {
            control.append(IPPROTO_IP, IP_TOS, static_cast<unsigned char>(*info.ecn));
        }
        else
        {
            control.append(IPPROTO_IPV6, IPV6_TCLASS, static_cast<int>(*info.ecn));
        }
#elif !defined(_WIN32)
        control.append(v4 ? IPPROTO_IP : IPPROTO_IPV6, v4 ? IP_TOS : IPV6_TCLASS, int(*info.ecn));
#else
        throw std::system_error(std::make_error_code(std::errc::operation_not_supported), "UDP ECN");
#endif
    }
}

#if defined(_WIN32)
template <class Function>
Function message_extension(native_socket_type fd, GUID id)
{
    Function fn{};
    DWORD size = 0;
    if (::WSAIoctl(fd, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof(id), &fn, sizeof(fn), &size, nullptr, nullptr)
        != 0)
    {
        net::throw_socket_error("WSAIoctl(message extension)");
    }
    return fn;
}
#endif

int send_message(
    native_socket_type fd,
    net::socket_address& target,
    std::span<const std::byte> bytes,
    const packet_info& info
)
{
    control_buffer control;
    encode_control(control, info, target.family());
    native_message msg{};
    auto* data = const_cast<char*>(reinterpret_cast<const char*>(bytes.data()));
    char empty = 0;
    if (bytes.empty())
    {
        data = &empty;
    }
#if defined(_WIN32)
    WSABUF vector{static_cast<ULONG>(bytes.size()), data};
    msg.name = target.data();
    msg.namelen = target.length;
    msg.lpBuffers = &vector;
    msg.dwBufferCount = 1;
    msg.Control = {
        static_cast<ULONG>(control.used),
        control.used ? reinterpret_cast<char*>(control.bytes.data()) : nullptr
    };
    DWORD count = 0;
    const auto send = message_extension<LPFN_WSASENDMSG>(fd, WSAID_WSASENDMSG);
    return send(fd, &msg, 0, &count, nullptr, nullptr) == 0 ? static_cast<int>(count) : -1;
#else
    iovec vector{data, bytes.size()};
    msg.msg_name = target.data();
    msg.msg_namelen = target.length;
    msg.msg_iov = &vector;
    msg.msg_iovlen = 1;
    msg.msg_control = control.used ? control.bytes.data() : nullptr;
    msg.msg_controllen = static_cast<socklen_t>(control.used);
    return static_cast<int>(::sendmsg(fd, &msg, 0));
#endif
}

int receive_message(
    native_socket_type fd,
    net::socket_address& sender,
    std::span<std::byte> bytes,
    bool& truncated,
    packet_info& info
)
{
    control_buffer control;
    native_message msg{};
#if defined(_WIN32)
    WSABUF vector{static_cast<ULONG>(bytes.size()), reinterpret_cast<char*>(bytes.data())};
    msg.name = sender.data();
    msg.namelen = sender.length;
    msg.lpBuffers = &vector;
    msg.dwBufferCount = 1;
    msg.Control = {static_cast<ULONG>(control.bytes.size()), reinterpret_cast<char*>(control.bytes.data())};
    DWORD size = 0;
    const auto receive = message_extension<LPFN_WSARECVMSG>(fd, WSAID_WSARECVMSG);
    if (receive(fd, &msg, &size, nullptr, nullptr) != 0)
    {
        if (WSAGetLastError() != WSAEMSGSIZE)
        {
            return -1;
        }
        msg.dwFlags |= MSG_TRUNC;
        size = static_cast<DWORD>(bytes.size());
    }
    const auto flags = msg.dwFlags;
    sender.length = msg.namelen;
    const int count = static_cast<int>(size);
#else
    iovec vector{bytes.data(), bytes.size()};
    msg.msg_name = sender.data();
    msg.msg_namelen = sender.length;
    msg.msg_iov = &vector;
    msg.msg_iovlen = 1;
    msg.msg_control = control.bytes.data();
    msg.msg_controllen = control.bytes.size();
    const int count = static_cast<int>(::recvmsg(fd, &msg, 0));
    if (count < 0)
    {
        return -1;
    }
    const int flags = msg.msg_flags;
    sender.length = msg.msg_namelen;
#endif
    if ((flags & MSG_CTRUNC) != 0)
    {
        throw std::system_error(std::make_error_code(std::errc::message_size), "truncated UDP control message");
    }
    truncated = (flags & MSG_TRUNC) != 0;
    info = decode_control(msg);
    return count;
}
} // namespace
} // namespace pace::udp

#undef PACE_CMSG_FIRST
#undef PACE_CMSG_NEXT
#undef PACE_CMSG_DATA
#undef PACE_CMSG_LEN
#undef PACE_CMSG_SPACE
