#pragma once

#include <pace/runtime/io_context.hpp>

namespace pace::net
{
// Use the same poller registration and cancellation path as a public readiness wait.
struct socket_wait : io_context::io_awaiter
{
    explicit socket_wait(io_context::io_awaiter&& wait) noexcept
        : io_awaiter(std::move(wait))
    {
    }

    std::error_code await_resume() noexcept
    {
        return resume_error();
    }
};

inline std::error_code socket_state(const io_context& io, native_socket_type fd, bool require_owner = false)
{
    if (io.stopped())
    {
        return std::make_error_code(std::errc::operation_canceled);
    }
    if (fd == invalid_socket)
    {
        return std::make_error_code(std::errc::bad_file_descriptor);
    }
    auto* running = io_context::current();
    if ((require_owner || running) && running != &io)
    {
        throw std::logic_error("socket operation used from a different io_context");
    }
    return {};
}
} // namespace pace::net
