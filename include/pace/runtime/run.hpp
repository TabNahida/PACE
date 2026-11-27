#pragma once

#include <pace/runtime/io_context.hpp>
#include <pace/runtime/this_context.hpp>

#include <functional>
#include <optional>
#include <type_traits>

namespace pace
{
/// Implementation support for pace::run, operation boundaries and task groups: invoking an entry factory, its
/// result type, where its outcome is recorded, and the root task of pace::run. Not part of the API.
struct entry_access
{
    /// Calls `factory(io)` if that is valid, otherwise `factory()`.
    template <class Factory>
    static auto invoke(Factory& factory, io_context& io)
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

    template <class Task>
    struct task_result;

    template <class T>
    struct task_result<task<T>>
    {
        using type = T;
    };

    /// result<task<T>> is T.
    template <class Task>
    using result = task_result<Task>::type;

    /// Where an entry's value or exception is recorded.
    template <class T>
    struct state
    {
        std::optional<T> value;
        std::exception_ptr failure;
        bool completed = false;
    };

    /// The root task of pace::run: awaits the factory's task, records the outcome in `outcome` and stops `io`.
    template <class T, class Factory>
    static task<void> run(io_context& io, Factory& factory, state<T>& outcome)
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
                co_await invoke(factory, io);
            }
            else
            {
                outcome.value.emplace(co_await invoke(factory, io));
            }
            outcome.completed = true;
        }
        catch (...)
        {
            // io_context suppresses shutdown cancellation; run must still report
            // cancellation of its entry task to the synchronous caller.
            outcome.failure = std::current_exception();
            throw;
        }
    }
};

template <>
struct entry_access::state<void>
{
    std::exception_ptr failure;
    bool completed = false;
};

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
    using result =
        entry_access::result<decltype(entry_access::invoke(std::declval<function&>(), std::declval<io_context&>()))>;
    if (io_context::current())
    {
        throw std::logic_error("nested pace::run is not supported");
    }
    function owned(std::forward<Factory>(factory));
    entry_access::state<result> state;
    io_context io; // Destroy roots before their borrowed factory/state on error.
    io.spawn(entry_access::run<result>(io, owned, state));
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
