#pragma once

#include <pace/net/socket_options.hpp>
#include "net/socket_address.hpp"
#include "net/socket_ops.hpp"

#if !defined(_WIN32)
#include <net/if.h>
#include <netinet/tcp.h>
#endif

namespace pace::net
{
template <typename T>
void set_native_option(native_socket_type fd, int level, int name, T value, const char* label)
{
    if (::setsockopt(fd, level, name, reinterpret_cast<const char*>(&value), sizeof(value)) != 0)
    {
        throw_socket_error(label);
    }
}

[[noreturn]] inline void unsupported_option(const char* name)
{
    throw std::system_error(std::make_error_code(std::errc::operation_not_supported), name);
}

inline void bind_interface(native_socket_type fd, address_family family, unsigned index)
{
#if defined(SO_BINDTODEVICE)
    (void)family;
    char name[IF_NAMESIZE]{};
    if (index && !::if_indextoname(index, name))
    {
        throw_socket_error("if_indextoname");
    }
    if (::setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, name, static_cast<socket_length>(std::strlen(name) + 1)) != 0)
    {
        throw_socket_error("SO_BINDTODEVICE");
    }
#elif defined(IP_BOUND_IF) && defined(IPV6_BOUND_IF)
    if (family == address_family::ipv4)
    {
        set_native_option(fd, IPPROTO_IP, IP_BOUND_IF, index, "IP_BOUND_IF");
    }
    else
    {
        set_native_option(fd, IPPROTO_IPV6, IPV6_BOUND_IF, index, "IPV6_BOUND_IF");
    }
#elif defined(IP_UNICAST_IF) && defined(IPV6_UNICAST_IF)
    if (family == address_family::ipv4)
    {
        set_native_option(fd, IPPROTO_IP, IP_UNICAST_IF, htonl(index), "IP_UNICAST_IF");
    }
    else
    {
        set_native_option(fd, IPPROTO_IPV6, IPV6_UNICAST_IF, index, "IPV6_UNICAST_IF");
    }
#else
    (void)fd;
    (void)family;
    (void)index;
    unsupported_option("interface_index");
#endif
}

inline void enable_listener_fast_open(native_socket_type fd, int backlog)
{
#if defined(TCP_FASTOPEN)
    // Linux takes a queue length. BSD and Winsock take a boolean.
#if !defined(__linux__)
    (void)backlog;
    constexpr int value = 1;
#else
    const int value = backlog;
#endif
    set_native_option(fd, IPPROTO_TCP, TCP_FASTOPEN, value, "listener TCP_FASTOPEN");
#else
    (void)fd;
    (void)backlog;
    unsupported_option("listener Fast Open");
#endif
}

inline void enable_client_fast_open(native_socket_type fd)
{
#if defined(TCP_FASTOPEN_CONNECT)
    set_native_option(fd, IPPROTO_TCP, TCP_FASTOPEN_CONNECT, 1, "TCP_FASTOPEN_CONNECT");
#elif defined(__APPLE__)
    // The client uses connectx. Darwin's TCP_FASTOPEN socket option is listener-only.
    (void)fd;
#elif defined(TCP_FASTOPEN)
    set_native_option(fd, IPPROTO_TCP, TCP_FASTOPEN, 1, "client TCP_FASTOPEN");
#else
    (void)fd;
    unsupported_option("client Fast Open");
#endif
}

inline void apply_tcp_options(native_socket_type fd, address_family family, const socket_options& opts)
{
    if (opts.interface_index)
    {
        bind_interface(fd, family, *opts.interface_index);
    }
    if (opts.mark)
    {
#if defined(SO_MARK)
        set_native_option(fd, SOL_SOCKET, SO_MARK, *opts.mark, "SO_MARK");
#elif defined(SO_USER_COOKIE)
        set_native_option(fd, SOL_SOCKET, SO_USER_COOKIE, *opts.mark, "SO_USER_COOKIE");
#else
        unsupported_option("socket mark");
#endif
    }
    if (opts.routing_table)
    {
#if defined(SO_SETFIB)
        set_native_option(fd, SOL_SOCKET, SO_SETFIB, *opts.routing_table, "SO_SETFIB");
#else
        unsupported_option("routing_table");
#endif
    }
    if (opts.keepalive)
    {
        set_native_option(fd, SOL_SOCKET, SO_KEEPALIVE, int(*opts.keepalive), "SO_KEEPALIVE");
    }
    if (opts.keepalive_idle_seconds)
    {
#if defined(TCP_KEEPIDLE)
        set_native_option(fd, IPPROTO_TCP, TCP_KEEPIDLE, *opts.keepalive_idle_seconds, "TCP_KEEPIDLE");
#elif defined(__APPLE__) && defined(TCP_KEEPALIVE)
        set_native_option(fd, IPPROTO_TCP, TCP_KEEPALIVE, *opts.keepalive_idle_seconds, "TCP_KEEPALIVE");
#else
        unsupported_option("keepalive_idle_seconds");
#endif
    }
    if (opts.keepalive_interval_seconds)
    {
#if defined(TCP_KEEPINTVL)
        set_native_option(fd, IPPROTO_TCP, TCP_KEEPINTVL, *opts.keepalive_interval_seconds, "TCP_KEEPINTVL");
#else
        unsupported_option("keepalive_interval_seconds");
#endif
    }
    if (opts.keepalive_count)
    {
#if defined(TCP_KEEPCNT)
        set_native_option(fd, IPPROTO_TCP, TCP_KEEPCNT, *opts.keepalive_count, "TCP_KEEPCNT");
#else
        unsupported_option("keepalive_count");
#endif
    }
    if (opts.user_timeout_ms)
    {
#if defined(TCP_USER_TIMEOUT)
        set_native_option(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, *opts.user_timeout_ms, "TCP_USER_TIMEOUT");
#else
        unsupported_option("TCP_USER_TIMEOUT");
#endif
    }
    if (opts.dont_fragment)
    {
        const int df = *opts.dont_fragment ? 1 : 0;
        if (family == address_family::ipv4)
        {
#if defined(IP_MTU_DISCOVER) && (defined(IP_PMTUDISC_DO) || defined(_WIN32))
            set_native_option(
                fd,
                IPPROTO_IP,
                IP_MTU_DISCOVER,
                df ? IP_PMTUDISC_DO : IP_PMTUDISC_DONT,
                "IP_MTU_DISCOVER"
            );
#elif defined(IP_DONTFRAG)
            set_native_option(fd, IPPROTO_IP, IP_DONTFRAG, df, "IP_DONTFRAG");
#elif defined(IP_DONTFRAGMENT)
            set_native_option(fd, IPPROTO_IP, IP_DONTFRAGMENT, df, "IP_DONTFRAGMENT");
#else
            (void)df;
            unsupported_option("IPv4 DF");
#endif
        }
        else
        {
#if defined(IPV6_MTU_DISCOVER) && defined(IPV6_PMTUDISC_DO)
            set_native_option(
                fd,
                IPPROTO_IPV6,
                IPV6_MTU_DISCOVER,
                df ? IPV6_PMTUDISC_DO : IPV6_PMTUDISC_DONT,
                "IPV6_MTU_DISCOVER"
            );
#elif defined(_WIN32) && defined(IPV6_MTU_DISCOVER)
            set_native_option(
                fd,
                IPPROTO_IPV6,
                IPV6_MTU_DISCOVER,
                df ? IP_PMTUDISC_DO : IP_PMTUDISC_DONT,
                "IPV6_MTU_DISCOVER"
            );
#elif defined(IPV6_DONTFRAG)
            set_native_option(fd, IPPROTO_IPV6, IPV6_DONTFRAG, df, "IPV6_DONTFRAG");
            // BSD kernels can accept this datagram option on TCP but leave it disabled.
            int applied = 0;
            socket_length size = sizeof(applied);
            if (::getsockopt(fd, IPPROTO_IPV6, IPV6_DONTFRAG, reinterpret_cast<char*>(&applied), &size) != 0)
            {
                throw_socket_error("getsockopt(IPV6_DONTFRAG)");
            }
            if (df && !applied)
            {
                unsupported_option("IPv6 DF on TCP");
            }
#else
            unsupported_option("IPv6 DF");
#endif
        }
    }
    if (opts.transparent)
    {
#if defined(IP_TRANSPARENT) && defined(IPV6_TRANSPARENT)
        if (family == address_family::ipv4)
        {
            set_native_option(fd, IPPROTO_IP, IP_TRANSPARENT, 1, "IP_TRANSPARENT");
        }
        else
        {
            set_native_option(fd, IPPROTO_IPV6, IPV6_TRANSPARENT, 1, "IPV6_TRANSPARENT");
        }
#else
        unsupported_option("transparent bind");
#endif
    }
}
} // namespace pace::net
