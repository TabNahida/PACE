#pragma once

#include "net/socket_address.hpp"
#include "net/socket_ops.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#endif

namespace pace::net
{
inline int socket_option(native_socket_type fd, int level, int option)
{
    int value = 0;
    socket_length size = sizeof(value);
    if (::getsockopt(fd, level, option, reinterpret_cast<char*>(&value), &size) != 0)
    {
        throw_socket_error("getsockopt");
    }
    return value;
}

inline address_family inspect_socket(native_socket_type fd, int type)
{
    if (fd == invalid_socket)
    {
        throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor));
    }
    if (socket_option(fd, SOL_SOCKET, SO_TYPE) != type)
    {
        throw std::invalid_argument("adopt: incompatible socket type");
    }
#if defined(_WIN32)
    // getsockname fails on unbound Winsock sockets.
    WSAPROTOCOL_INFOW info{};
    int length = sizeof(info);
    if (::getsockopt(fd, SOL_SOCKET, SO_PROTOCOL_INFOW, reinterpret_cast<char*>(&info), &length) != 0)
    {
        throw_socket_error("getsockopt(SO_PROTOCOL_INFO)");
    }
    const auto family = info.iAddressFamily;
    const auto protocol = info.iProtocol;
#else
    socket_address local;
    if (::getsockname(fd, local.data(), &local.length) != 0)
    {
        throw_socket_error("getsockname");
    }
    const auto family = local.native_family();
#if defined(SO_PROTOCOL)
    const auto protocol = socket_option(fd, SOL_SOCKET, SO_PROTOCOL);
#endif
#endif
    if (family != AF_INET && family != AF_INET6)
    {
        throw std::system_error(std::make_error_code(std::errc::address_family_not_supported));
    }
#if defined(_WIN32) || defined(SO_PROTOCOL)
    if (protocol != (type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP))
    {
        throw std::invalid_argument("adopt: incompatible socket protocol");
    }
#endif
    return family == AF_INET ? address_family::ipv4 : address_family::ipv6;
}

inline void configure_socket(native_socket_type fd)
{
#if defined(_WIN32)
    u_long nonblocking = 1;
    if (::ioctlsocket(fd, FIONBIO, &nonblocking) != 0)
    {
        throw_socket_error("ioctlsocket(FIONBIO)");
    }
    if (!::SetHandleInformation(reinterpret_cast<HANDLE>(fd), HANDLE_FLAG_INHERIT, 0))
    {
        throw std::system_error(::GetLastError(), std::system_category(), "SetHandleInformation(socket)");
    }
#else
    const int flags = ::fcntl(fd, F_GETFL, 0);
    const int descriptor_flags = ::fcntl(fd, F_GETFD, 0);
    if (flags < 0 || descriptor_flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0
        || ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0)
    {
        throw_socket_error("fcntl(nonblocking/close-on-exec)");
    }
#endif
}

inline void configure_stream(native_socket_type fd)
{
    configure_socket(fd);
#if defined(SO_NOSIGPIPE)
    const int yes = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) != 0)
    {
        throw_socket_error("setsockopt(SO_NOSIGPIPE)");
    }
#endif
}
} // namespace pace::net
