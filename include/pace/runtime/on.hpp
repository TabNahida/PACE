#pragma once
#include <pace/runtime/operation.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace pace
{
/// Implementation support for on(): the state shared by the caller and the work, the root task that runs the work
/// on the target, and the caller's coroutine. Not part of the API.
struct on_access
{
    /// The outcome of the work and how to resume the caller.
    template <class T>
    struct operation
    {
        enum : int
        {
            running,
            waiting,
            done
        };

        cancellation_source source;
        entry_access::state<T> outcome;
        std::shared_ptr<remote_access::anchor> anchor;
        std::coroutine_handle<> continuation;
        std::atomic<int> phase{running};

        explicit operation(deadline expires)
            : source(expires)
        {
        }

        /// Records operation_canceled unless the work produced an outcome, then resumes a waiting caller.
        void finish() noexcept
        {
            if (!outcome.completed && !outcome.failure)
            {
                outcome.failure =
                    std::make_exception_ptr(std::system_error(std::make_error_code(std::errc::operation_canceled)));
            }
            if (phase.exchange(done, std::memory_order_acq_rel) == waiting)
            {
                remote_access::resume(*anchor, continuation);
            }
        }
    };

    /// The parameter of the root task: owns the factory and finishes the operation when the root's frame goes,
    /// whether it ran to completion or was destroyed unstarted or suspended with the target.
    template <class T, class Factory>
    struct owned
    {
        std::shared_ptr<operation<T>> state;
        std::optional<Factory> factory;

        owned(std::shared_ptr<operation<T>> shared, Factory work)
            : state(std::move(shared))
            , factory(std::move(work))
        {
        }

        owned(owned&& other) noexcept(std::is_nothrow_move_constructible_v<Factory>)
            : state(std::move(other.state))
            , factory(std::move(other.factory))
        {
            other.factory.reset();
        }

        owned(const owned&) = delete;
        owned& operator=(const owned&) = delete;
        owned& operator=(owned&&) = delete;

        ~owned()
        {
            factory.reset(); // Owned captures go before the caller resumes.
            if (state)
            {
                state->finish();
            }
        }
    };

    /// The root task on the target: runs the factory's task under the operation's token.
    template <class T, class Factory>
    static task<void> run(owned<T, Factory> work)
    {
        auto& op = *work.state;
        try
        {
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): engaged until ~owned
            auto child = task_access::bind(entry_access::invoke(*work.factory, this_context()), op.source.token());
            if constexpr (std::is_void_v<T>)
            {
                co_await std::move(child);
            }
            else
            {
                op.outcome.value.emplace(co_await std::move(child));
            }
            op.outcome.completed = true;
        }
        catch (...)
        {
            op.outcome.failure = std::current_exception();
        }
    }

    /// Suspends the caller until the work has finished.
    template <class T>
    struct wait
    {
        operation<T>& op;

        bool await_ready() const noexcept
        {
            return op.phase.load(std::memory_order_acquire) == operation<T>::done;
        }

        bool await_suspend(std::coroutine_handle<> handle) noexcept
        {
            op.continuation = handle;
            int expected = operation<T>::running;
            return op.phase.compare_exchange_strong(
                expected,
                operation<T>::waiting,
                std::memory_order_acq_rel,
                std::memory_order_acquire
            );
        }

        void await_resume() const noexcept
        {
        }
    };

    /// The caller's side of on().
    template <class Factory>
    static task<operation_result<Factory>> call(io_context& target, Factory factory)
    {
        using result = operation_result<Factory>;
        auto& here = this_context();
        if (&target == &here)
        {
            if constexpr (std::is_void_v<result>)
            {
                co_await entry_access::invoke(factory, here);
                co_return;
            }
            else
            {
                co_return co_await entry_access::invoke(factory, here);
            }
        }
        auto token = co_await this_task::cancellation();
        token.throw_if_cancelled();
        auto op = std::make_shared<operation<result>>(token.expires());
        op->source.link(token);
        op->anchor = remote_access::anchor_of(here);
        // The token reaches the work through link() at once; this registration adds the caller's own stop.
        auto registration = remote_access::on_cancel_or_stop(
            here,
            token,
            [state = op.get()](std::error_code reason)
            {
                if (reason == std::errc::timed_out)
                {
                    state->source.request_timeout();
                }
                else
                {
                    state->source.request_cancellation();
                }
            }
        );
        target.spawn(run(owned<result, Factory>(op, std::move(factory))));
        co_await wait<result>{*op};
        registration.cancel();
        if (op->outcome.failure)
        {
            std::rethrow_exception(op->outcome.failure);
        }
        if constexpr (!std::is_void_v<result>)
        {
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): completed without failure, so the value is set
            co_return std::move(*op->outcome.value);
        }
    }
};

/// Runs the factory's task on `target` and resumes the caller on its own context with the result or exception.
/// The factory (taking io_context& or nothing) is moved to and invoked on `target`, owned until the work finishes,
/// and destroyed there before the caller resumes. The work runs under a token linked to the caller's, with the
/// caller's deadline; the caller's cancellation, deadline and context stop cancel it, and the caller always waits
/// for it to finish. On the caller's own context the factory's task is simply awaited. Throws
/// std::system_error(operation_canceled) if `target` is stopped (the work never runs) or is destroyed before the
/// work finished, and otherwise what the work threw. Call on a running context.
template <class Factory>
requires(std::is_invocable_v<Factory&, io_context&> || std::is_invocable_v<Factory&>)
auto on(io_context& target, Factory factory)
{
    return on_access::call(target, std::move(factory));
}

/// on() for an already-created task: the task's frame moves to `target`; what it borrowed stays borrowed and must
/// stay valid until the co_await completes.
template <class T>
auto on(io_context& target, task<T> work)
{
    return on(target, [work = std::move(work)]() mutable { return std::move(work); });
}
} // namespace pace
