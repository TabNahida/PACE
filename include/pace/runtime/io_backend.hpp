#pragma once

namespace pace
{
/// The readiness poller of an io_context. An explicit choice that is unavailable makes construction throw
/// std::system_error(not_supported); there is no silent fallback.
enum class io_backend
{
    automatic, ///< PACE_IO_BACKEND if set, otherwise the platform default.
    epoll,     ///< Linux default.
    kqueue,    ///< macOS and FreeBSD default.
    wsapoll,   ///< Windows fallback; always used by 32-bit builds.
    io_uring,  ///< Linux built with --uring=y; explicit choice only.
    iocp       ///< 64-bit Windows build 20348+; Windows default when available.
};

/// The lower-case name of `value`, as accepted by PACE_IO_BACKEND.
inline const char* backend_name(io_backend value) noexcept
{
    switch (value)
    {
        case io_backend::epoll:
            return "epoll";
        case io_backend::kqueue:
            return "kqueue";
        case io_backend::wsapoll:
            return "wsapoll";
        case io_backend::io_uring:
            return "io_uring";
        case io_backend::iocp:
            return "iocp";
        default:
            return "automatic";
    }
}
} // namespace pace
