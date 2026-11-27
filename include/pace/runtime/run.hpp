#pragma once

#include <pace/runtime/io_context.hpp>
#include <pace/runtime/this_context.hpp>

#include <functional>
#include <optional>
#include <type_traits>

namespace pace
{
/// Calls `factory(io)` if that is valid, otherwise `factory()`.
template <class Factory>
auto invoke_entry(Factory& factory, io_context& io)
{
    if constexpr (std::is_invocable_v<Factory&, io_context&>)
    {
        return std::invoke(factory, io);
    }
    else
    {
        return std::invoke(factory);
    }
}
/// entry_result<task<T>>::type is T: the result type of an entry factory's task.
template <class T>
struct entry_result;

template <class T>
struct entry_result<task<T>>
{
    using type = T;
};

/// Where run_entry records the entry's value or exception.
template <class T>
struct entry_state
{
    std::optional<T> value;
    std::exception_ptr failure;
    bool completed = false;
};

template <>
struct entry_state<void>
{
    std::exception_ptr failure;
    bool completed = false;
};

/// The root task of pace::run: awaits the factory's task, records the outcome in `state` and stops `io`.
template <class T, class Factory>
task<void> run_entry(io_context& io, Factory& factory, entry_state<T>& state)
{
    struct stop_on_exit
    {
        io_context& io;

        explicit stop_on_exit(io_context& context) noexcept
            : io(context)
        {
        }

        stop_on_exit(const stop_on_exit&) = delete;
        stop_on_exit& operator=(const stop_on_exit&) = delete;

        ~stop_on_exit()
        {
            io.stop();
        }
    } stop{io};

    try
    {
        if constexpr (std::is_void_v<T>)
        {
            co_await invoke_entry(factory, io);
        }
        else
        {
            state.value.emplace(co_await invoke_entry(factory, io));
        }
        state.completed = true;
    }
    catch (...)
    {
        // io_context suppresses shutdown cancellation; run must still report
        // cancellation of its entry task to the synchronous caller.
        state.failure = std::current_exception();
        throw;
    }
}

/// Runs the factory's task on a fresh io_context on this thread and returns its result. The factory (taking
/// io_context& or nothing) is owned until shutdown finishes, so a capturing coroutine lambda may be passed directly.
/// Entry completion stops the context and drains the cancellation of background roots. Return owned data, not
/// sockets or views into the context or the factory. Throws std::logic_error on a thread already running a context,
/// rethrows the entry's or a background root's exception, and throws operation_canceled if the entry did not finish.
template <class Factory>
requires(std::is_invocable_v<std::decay_t<Factory>&, io_context&> || std::is_invocable_v<std::decay_t<Factory>&>)
auto run(Factory&& factory)
{
    using function = std::decay_t<Factory>;
    using result = entry_result<decltype(invoke_entry(std::declval<function&>(), std::declval<io_context&>()))>::type;
    if (io_context::current())
    {
        throw std::logic_error("nested pace::run is not supported");
    }
    function owned(std::forward<Factory>(factory));
    entry_state<result> state;
    io_context io; // Destroy roots before their borrowed factory/state on error.
    io.spawn(run_entry<result>(io, owned, state));
    io.run();
    if (state.failure)
    {
        std::rethrow_exception(state.failure);
    }
    // The entry stores its value before it sets completed, so for a value-returning
    // entry both conditions agree; testing the optional makes the access visibly safe.
    if constexpr (std::is_void_v<result>)
    {
        if (!state.completed)
        {
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        }
    }
    else
    {
        if (!state.completed || !state.value)
        {
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        }
        return result(std::move(*state.value));
    }
}

/// run() for an already-created task. The task owns its arguments, but its references and lambda captures stay
/// borrowed: pass a capturing coroutine lambda itself to run(), not lambda().
template <class T>
auto run(task<T> entry)
{
    return run([entry = std::move(entry)]() mutable { return std::move(entry); });
}
} // namespace pace
