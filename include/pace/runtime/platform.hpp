#pragma once

#include <pace/runtime/requirements.hpp>

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

namespace pace
{
/// A native socket: a file descriptor, or a SOCKET value on Windows.
#ifdef _WIN32
using native_socket_type = std::uintptr_t;
#else
using native_socket_type = int;
#endif
/// "No socket". Waiting on it fails with std::errc::bad_file_descriptor.
inline constexpr native_socket_type invalid_socket = static_cast<native_socket_type>(-1);

#ifdef _WIN32
/// Creates an overlapped, non-inheritable socket with WSASocketW; returns INVALID_SOCKET on failure.
inline SOCKET create_windows_socket(int family, int type, int protocol) noexcept
{
    return ::WSASocketW(family, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
}
#endif

/// Starts Winsock 2.2 once per process on Windows (thread-safe); does nothing elsewhere.
/// Throws std::system_error if WSAStartup fails. PACE calls it before creating any socket.
inline void initialize_sockets()
{
#ifdef _WIN32
    struct socket_runtime
    {
        socket_runtime()
        {
            WSADATA data{};
            const int error = ::WSAStartup(MAKEWORD(2, 2), &data);
            if (error)
            {
                throw std::system_error(error, std::system_category(), "WSAStartup");
            }
        }

        ~socket_runtime()
        {
            ::WSACleanup();
        }
    };

    static socket_runtime runtime;
    (void)runtime;
#endif
}

/// Another name for initialize_sockets().
inline void initialize_winsock()
{
    initialize_sockets();
}
} // namespace pace
