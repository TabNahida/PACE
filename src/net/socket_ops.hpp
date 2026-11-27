#pragma once

// Internal: the platform socket primitives TCP and UDP share.
#include <pace/runtime/platform.hpp>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <cerrno>

#include <sys/socket.h>
#include <unistd.h>
#endif

#include <stdexcept>
#include <system_error>

namespace pace::net
{
#if defined(_WIN32)
using socket_length = int;

inline int last_socket_error() noexcept
{
    return ::WSAGetLastError();
}

inline void close_socket(native_socket_type fd) noexcept
{
    ::closesocket(fd);
}

inline bool interrupted(int error) noexcept
{
    return error == WSAEINTR;
}

inline bool would_block(int error) noexcept
{
    return error == WSAEWOULDBLOCK;
}

inline std::error_code socket_error_code(int error)
{
    return {error, std::system_category()};
}
#else
using socket_length = socklen_t;

inline int last_socket_error() noexcept
{
    return errno;
}

inline void close_socket(native_socket_type fd) noexcept
{
    ::close(fd);
}

inline bool interrupted(int error) noexcept
{
    return error == EINTR;
}

inline bool would_block(int error) noexcept
{
    return error == EAGAIN || error == EWOULDBLOCK;
}

inline std::error_code socket_error_code(int error)
{
    return {error, std::generic_category()};
}
#endif

// Throws the last socket error of this thread.
[[noreturn]] inline void throw_socket_error(const char* operation)
{
    throw std::system_error(socket_error_code(last_socket_error()), operation);
}

// Marks one direction of a socket busy for the lifetime of an operation. A second operation in the same direction
// is rejected before it touches the socket: once the first one is suspended, a second reader could otherwise take
// the data the first is waiting for, and a second writer could interleave its bytes when the first yields.
class pending_operation
{
public:
    pending_operation(bool& active, const char* message)
        : active_(active)
    {
        if (active_)
        {
            throw std::logic_error(message);
        }
        active_ = true;
    }

    pending_operation(const pending_operation&) = delete;
    pending_operation& operator=(const pending_operation&) = delete;

    ~pending_operation()
    {
        active_ = false;
    }

private:
    bool& active_;
};
} // namespace pace::net
