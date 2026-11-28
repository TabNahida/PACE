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

std::error_code io_context::io_awaiter::resume_error() noexcept
{
    if (token.stop_possible())
    {
        registration.cancel();
        if (auto reason = token.error())
        {
            return reason;
        }
    }
    return !error && context.stopped() ? canceled() : error;
}

void io_context::io_awaiter::await_resume()
{
    if (auto reason = resume_error())
    {
        throw std::system_error(reason);
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
        throw std::logic_error("only one pending operation per descriptor direction is allowed");
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

#ifdef _WIN32
// wait_handle: one thread-pool wait per operation. Its callback runs on a pool thread and posts the operation's key
// to the context, which wakes the poller on either backend (IOCP: a completion packet; WSAPoll: the loopback
// datagram). The pool's wait thread is the WSAPoll backend's helper thread, and IOCP needs no special packet key.
bool io_context::handle_awaiter::await_ready() noexcept
{
    if (context.stopped())
    {
        error = canceled();
        return true;
    }
    return false;
}

void io_context::handle_awaiter::await_resume()
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

void CALLBACK
io_context::implementation::handle_signaled(PTP_CALLBACK_INSTANCE, PVOID parameter, PTP_WAIT, TP_WAIT_RESULT) noexcept
{
    // The operation stays alive while its wait exists: completion and cancellation both wait for this callback to
    // return before the reactor resumes the coroutine. Copy what is needed before posting.
    const auto& operation = *static_cast<const handle_awaiter*>(parameter);
    auto& io = operation.context;
    const auto id = operation.id;
    try
    {
        io.post([&io, id] { io.impl_->complete_handle_wait(id); });
    }
    catch (...)
    {
        // Stopped: shutdown cancels the wait. Out of memory: the wait stays pending until it is cancelled.
    }
}

void io_context::implementation::complete_handle_wait(std::uint64_t id) noexcept
{
    const auto it = handle_waits.find(id);
    if (it == handle_waits.end())
    {
        return;
    }
    auto& operation = *it->second;
    handle_waits.erase(it);
    auto* wait = static_cast<PTP_WAIT>(std::exchange(operation.wait, nullptr));
    // The callback that posted this may still be returning from post(); let it finish before the frame resumes.
    WaitForThreadpoolWaitCallbacks(wait, FALSE);
    CloseThreadpoolWait(wait);
    ready.push_back(operation.continuation);
}

void io_context::implementation::cancel_handle_wait(handle_awaiter& operation, std::error_code error) noexcept
{
    const auto it = handle_waits.find(operation.id);
    if (it == handle_waits.end() || it->second != &operation)
    {
        return;
    }
    handle_waits.erase(it);
    auto* wait = static_cast<PTP_WAIT>(std::exchange(operation.wait, nullptr));
    // Stop new callbacks, drop a queued one and wait for a running one. A callback that already posted the key
    // finds it gone and does nothing.
    SetThreadpoolWait(wait, nullptr, nullptr);
    WaitForThreadpoolWaitCallbacks(wait, TRUE);
    CloseThreadpoolWait(wait);
    operation.error = error;
    ready.push_back(operation.continuation);
}

bool io_context::arm(handle_awaiter& operation, std::coroutine_handle<> handle)
{
    check_affinity(this);
    if (stopped())
    {
        operation.error = canceled();
        return false;
    }
    if (!operation.object || operation.object == INVALID_HANDLE_VALUE)
    {
        operation.error = std::make_error_code(std::errc::bad_file_descriptor);
        return false;
    }
    auto& p = *impl_;
    if (p.next_handle_wait == UINT64_MAX)
    {
        throw std::overflow_error("handle wait identity exhausted");
    }
    auto* wait = CreateThreadpoolWait(&implementation::handle_signaled, &operation, nullptr);
    if (!wait)
    {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "CreateThreadpoolWait");
    }
    operation.id = ++p.next_handle_wait;
    try
    {
        p.handle_waits.emplace(operation.id, &operation);
    }
    catch (...)
    {
        CloseThreadpoolWait(wait);
        throw;
    }
    operation.wait = wait;
    operation.continuation = handle;
    SetThreadpoolWait(wait, operation.object, nullptr);
    return true;
}

void io_context::cancel_wait(handle_awaiter& operation, std::error_code error)
{
    impl_->cancel_handle_wait(operation, error);
}
#endif
} // namespace pace
