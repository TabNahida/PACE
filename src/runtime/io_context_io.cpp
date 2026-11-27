// Readiness waits on sockets and handles, and their cancellation.
#include "runtime/io_context_state.hpp"

namespace pace
{
bool io_context::io_awaiter::await_ready() noexcept
{
    if (context.stopped())
    {
        error = canceled();
        return true;
    }
    return false;
}

void io_context::io_awaiter::await_resume()
{
    if (token.stop_possible())
    {
        registration.cancel();
        token.throw_if_cancelled();
    }
    if (error)
    {
        throw std::system_error(error);
    }
    if (context.stopped())
    {
        throw std::system_error(canceled());
    }
}

bool io_context::arm(io_awaiter& operation, std::coroutine_handle<> handle)
{
    profiling::scope timing(profiling::stage::io_arm);
    check_affinity(this);
    if (stopped())
    {
        operation.error = canceled();
        return false;
    }
    if (operation.fd == invalid_socket)
    {
        operation.error = std::make_error_code(std::errc::bad_file_descriptor);
        return false;
    }
    auto& p = *impl_;
    auto token_it = p.tokens.find(operation.fd);
    const bool inserted = token_it == p.tokens.end();
    std::uint64_t token;
    if (inserted)
    {
        token = p.add_descriptor(operation.fd);
        try
        {
            p.tokens.emplace(operation.fd, token);
        }
        catch (...)
        {
            p.erase_descriptor(token);
            throw;
        }
    }
    else
    {
        token = token_it->second;
    }
    auto& state = *p.find_descriptor(token);
    const bool read = !operation.writable || operation.either;
    const bool write = operation.writable || operation.either;
    if ((read && state.reader) || (write && state.writer))
    {
        throw std::logic_error("only one pending operation per socket direction is allowed");
    }
    operation.continuation = handle;
    if (read)
    {
        state.reader = &operation;
    }
    if (write)
    {
        state.writer = &operation;
    }
    try
    {
        p.update(state, token);
    }
    catch (...)
    {
        if (read)
        {
            state.reader = nullptr;
        }
        if (write)
        {
            state.writer = nullptr;
        }
        if (inserted)
        {
            p.tokens.erase(operation.fd);
            p.erase_descriptor(token);
        }
        throw;
    }
    return true;
}

void io_context::cancel(native_socket_type fd) noexcept
{
    auto& p = *impl_;
    const auto it = p.tokens.find(fd);
    if (it == p.tokens.end())
    {
        return;
    }
    auto& state = *p.find_descriptor(it->second);
    p.poller.remove(fd);
    p.finish(state, state.reader, canceled());
    p.finish(state, state.writer, canceled());
    p.erase_descriptor(it->second);
    p.tokens.erase(it);
}

void io_context::release_socket(native_socket_type fd) noexcept
{
    cancel(fd);
    impl_->poller.forget(fd);
}
} // namespace pace
