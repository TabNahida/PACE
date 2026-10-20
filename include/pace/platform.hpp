#pragma once

#include <cstdint>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace pace {

#ifdef _WIN32
using native_socket_type = std::uintptr_t;
#else
using native_socket_type = int;
#endif
inline constexpr native_socket_type invalid_socket = static_cast<native_socket_type>(-1);

namespace detail {

// Function-local initialization is thread-safe and precedes every owned socket.
inline void initialize_sockets() {
#ifdef _WIN32
    struct socket_runtime {
        socket_runtime() {
            WSADATA data{};
            const int error = ::WSAStartup(MAKEWORD(2, 2), &data);
            if (error) throw std::system_error(error, std::system_category(), "WSAStartup");
        }
        ~socket_runtime() { ::WSACleanup(); }
    };
    static socket_runtime runtime;
    (void)runtime;
#endif
}

inline void initialize_winsock() { initialize_sockets(); }

} // namespace detail
} // namespace pace
