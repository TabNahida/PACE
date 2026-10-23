#pragma once

#include <pace/ip.hpp>
#include <pace/platform.hpp>
#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

namespace pace::detail {

#if defined(_WIN32)
using socket_address_length = int;
#else
using socket_address_length = socklen_t;
#endif

struct socket_address {
    sockaddr_storage storage{};
    socket_address_length length = sizeof(storage);

    sockaddr* data() noexcept { return reinterpret_cast<sockaddr*>(&storage); }
    const sockaddr* data() const noexcept { return reinterpret_cast<const sockaddr*>(&storage); }
    int native_family() const noexcept { return storage.ss_family; }
    address_family family() const noexcept {
        if (storage.ss_family == AF_INET) return address_family::ipv4;
        if (storage.ss_family == AF_INET6) return address_family::ipv6;
        return address_family::unspecified;
    }
};

inline int native_address_family(address_family family) {
    if (family == address_family::ipv4) return AF_INET;
    if (family == address_family::ipv6) return AF_INET6;
    throw std::invalid_argument("expected IPv4 or IPv6 socket family");
}

inline socket_address parse_socket_address(const std::string& text, std::uint16_t port,
        address_family family = address_family::unspecified, std::uint32_t scope_id = 0) {
    if (family != address_family::unspecified && family != address_family::ipv4 && family != address_family::ipv6)
        throw std::invalid_argument("invalid endpoint address family");
    if (text.find('%') != std::string::npos)
        throw std::invalid_argument("IPv6 zone suffixes are not accepted; use endpoint.scope_id");
    if (text.empty() || text.find('\0') != std::string::npos)
        throw std::invalid_argument("expected a numeric IPv4 or IPv6 address");
    socket_address result;
    sockaddr_in v4{};
    v4.sin_family = AF_INET;
    v4.sin_port = htons(port);
#if defined(__APPLE__) || defined(__FreeBSD__)
    v4.sin_len = sizeof(v4);
#endif
    if (::inet_pton(AF_INET, text.c_str(), &v4.sin_addr) == 1) {
        if (family == address_family::ipv6 || scope_id != 0)
            throw std::invalid_argument("IPv4 endpoint has an incompatible family or scope");
        result.length = sizeof(v4);
        std::memcpy(&result.storage, &v4, sizeof(v4));
        return result;
    }
    sockaddr_in6 v6{};
    v6.sin6_family = AF_INET6;
    v6.sin6_port = htons(port);
    v6.sin6_scope_id = scope_id;
#if defined(__APPLE__) || defined(__FreeBSD__)
    v6.sin6_len = sizeof(v6);
#endif
    if (::inet_pton(AF_INET6, text.c_str(), &v6.sin6_addr) != 1)
        throw std::invalid_argument("expected a numeric IPv4 or IPv6 address");
    if (family == address_family::ipv4)
        throw std::invalid_argument("IPv6 endpoint has an incompatible family");
    result.length = sizeof(v6);
    std::memcpy(&result.storage, &v6, sizeof(v6));
    return result;
}

// Preserve mapped IPv4 addresses as IPv6, including on accepted dual-stack
// connections. Callers doing access control must account for mapped addresses.
template<class Endpoint>
Endpoint socket_endpoint(const sockaddr* address, socket_address_length length) {
    char text[INET6_ADDRSTRLEN]{};
    Endpoint result;
    const void* bytes = nullptr;
    sockaddr_in v4{};
    sockaddr_in6 v6{};
    if (!address || length < static_cast<socket_address_length>(offsetof(sockaddr, sa_family) + sizeof(address->sa_family)))
        throw std::invalid_argument("truncated native socket address");
    if (address->sa_family == AF_INET && length >= static_cast<socket_address_length>(sizeof(v4))) {
        std::memcpy(&v4, address, sizeof(v4));
        bytes = &v4.sin_addr; result.port = ntohs(v4.sin_port);
        result.family = address_family::ipv4; result.scope_id = 0;
    } else if (address->sa_family == AF_INET6 && length >= static_cast<socket_address_length>(sizeof(v6))) {
        std::memcpy(&v6, address, sizeof(v6));
        bytes = &v6.sin6_addr; result.port = ntohs(v6.sin6_port);
        result.family = address_family::ipv6; result.scope_id = v6.sin6_scope_id;
    } else throw std::invalid_argument("unsupported or truncated native socket address");
    if (!::inet_ntop(address->sa_family, bytes, text, sizeof(text))) {
#if defined(_WIN32)
        throw std::system_error(::WSAGetLastError(), std::system_category(), "inet_ntop");
#else
        throw std::system_error(errno, std::generic_category(), "inet_ntop");
#endif
    }
    result.address = text;
    return result;
}

inline socket_address map_ipv4_address(const socket_address& address) {
    if (address.native_family() != AF_INET) return address;
    sockaddr_in v4{};
    std::memcpy(&v4, &address.storage, sizeof(v4));
    sockaddr_in6 v6{};
    v6.sin6_family = AF_INET6;
    v6.sin6_port = v4.sin_port;
#if defined(__APPLE__) || defined(__FreeBSD__)
    v6.sin6_len = sizeof(v6);
#endif
    auto* bytes = reinterpret_cast<unsigned char*>(&v6.sin6_addr);
    bytes[10] = bytes[11] = 0xff;
    std::memcpy(bytes + 12, &v4.sin_addr, sizeof(v4.sin_addr));
    socket_address result;
    result.length = sizeof(v6);
    std::memcpy(&result.storage, &v6, sizeof(v6));
    return result;
}

} // namespace pace::detail
