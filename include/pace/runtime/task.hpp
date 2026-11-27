#pragma once

#include <pace/runtime/cancellation.hpp>

#include <coroutine>
#include <cstddef>
#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace pace
{
struct task_access;

/// The promise base of every task: lazy start, stored exception, continuation and the frame's cancellation token.
/// Fields other than `cancellation` are io_context bookkeeping.
struct promise_base
{
    cancellation_token cancellation;
    bool cancellation_bound = false;
    std::coroutine_handle<> continuation = std::noop_coroutine();
    std::exception_ptr exception;
    std::coroutine_handle<> completed_next{};
    void* completion_context = nullptr;
    void (*on_completion)(void*, std::coroutine_handle<>) noexcept = nullptr;

    /// Every task starts suspended.
    std::suspend_always initial_suspend() const noexcept
    {
        return {};
    }

    /// Stores the escaping exception; the awaiter rethrows it.
    void unhandled_exception() noexcept
    {
        exception = std::current_exception();
    }

    /// Notifies a root's owner, then resumes the continuation.
    struct final_awaiter
    {
        bool await_ready() const noexcept
        {
            return false;
        }

        template <class P>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) const noexcept
        {
            auto& p = h.promise();
            if (p.on_completion)
            {
                p.on_completion(p.completion_context, h);
            }
            return p.continuation;
        }

        void await_resume() const noexcept
        {
        }
    };

    /// Transfers control to the awaiting parent, if any.
    final_awaiter final_suspend() const noexcept
    {
        return {};
    }
};

/// Lazy, move-only, single-consumer coroutine. The body starts when the task is awaited or spawned.
/// `co_await std::move(task)` transfers ownership; parameters passed by value live in the frame, references do not.
template <class T = void>
class [[nodiscard]] task
{
public:
    /// The coroutine promise; used by the compiler.
    struct promise_type : promise_base
    {
        std::optional<T> value;

        /// The task that owns the new frame.
        task get_return_object() noexcept
        {
            return task{handle_type::from_promise(*this)};
        }

        /// Stores the operand of co_return.
        template <class U>
        void return_value(U&& result)
        {
            value.emplace(std::forward<U>(result));
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;
    /// An empty task; awaiting it throws std::logic_error.
    task() noexcept = default;

    /// Takes the frame; `other` becomes empty.
    task(task&& other) noexcept
        : handle_(std::exchange(other.handle_, {}))
    {
    }

    /// Destroys the frame this task owned, then takes `other`'s.
    task& operator=(task&& other) noexcept
    {
        if (this != &other)
        {
            if (handle_)
            {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    task(const task&) = delete;
    task& operator=(const task&) = delete;

    /// Destroys an owned frame without running an unstarted body.
    ~task()
    {
        if (handle_)
        {
            handle_.destroy();
        }
    }

    /// Empties the task and returns its raw handle. The caller must resume, complete and destroy it exactly once.
    handle_type release() noexcept
    {
        return std::exchange(handle_, {});
    }

    /// Owns the frame for one co_await and destroys it afterwards.
    struct awaiter
    {
        handle_type handle;

        awaiter(handle_type h) noexcept
            : handle(h)
        {
        }

        awaiter(awaiter&& other) noexcept
            : handle(std::exchange(other.handle, {}))
        {
        }

        awaiter(const awaiter&) = delete;
        awaiter& operator=(const awaiter&) = delete;
        awaiter& operator=(awaiter&&) = delete;

        ~awaiter()
        {
            if (handle)
            {
                handle.destroy();
            }
        }

        /// True for an empty or finished frame.
        bool await_ready() const noexcept
        {
            return !handle || handle.done();
        }

        /// Passes the parent's token on (unless the child has a bound one), throws std::system_error if it is
        /// already cancelled, and transfers control to the child.
        template <class P>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<P> parent)
        {
            auto& promise = handle.promise();
            if (!promise.cancellation_bound)
            {
                if constexpr (requires { parent.promise().cancellation; })
                {
                    promise.cancellation = parent.promise().cancellation;
                }
            }
            promise.cancellation.throw_if_cancelled();
            handle.promise().continuation = parent;
            return handle;
        }

        /// Rethrows the child's exception, throws std::system_error if its token was cancelled by the time it
        /// finished, else moves the result out. Throws std::logic_error for an empty task.
        T await_resume()
        {
            if (!handle)
            {
                throw std::logic_error("cannot await an empty task");
            }
            if (handle.promise().exception)
            {
                std::rethrow_exception(handle.promise().exception);
            }
            handle.promise().cancellation.throw_if_cancelled();
            // A task<T> that finished without an exception ran return_value. Checking again
            // would add a branch to every co_await.
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): set by return_value, see above
            return std::move(*handle.promise().value);
        }
    };

    /// Only an rvalue can be awaited: write `co_await std::move(work)`.
    awaiter operator co_await() && noexcept
    {
        return awaiter{release()};
    }

private:
    friend struct task_access;

    explicit task(handle_type handle) noexcept
        : handle_(handle)
    {
    }

    handle_type handle_{};
};

template <>
class [[nodiscard]] task<void>
{
public:
    /// The coroutine promise; used by the compiler.
    struct promise_type : promise_base
    {
        std::size_t root_index = 0; ///< Used only while owned by io_context::spawn.

        /// The task that owns the new frame.
        task get_return_object() noexcept
        {
            return task{handle_type::from_promise(*this)};
        }

        /// Completes a task<void>.
        void return_void() const noexcept
        {
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;
    /// An empty task; awaiting it throws std::logic_error.
    task() noexcept = default;

    /// Takes the frame; `other` becomes empty.
    task(task&& other) noexcept
        : handle_(std::exchange(other.handle_, {}))
    {
    }

    /// Destroys the frame this task owned, then takes `other`'s.
    task& operator=(task&& other) noexcept
    {
        if (this != &other)
        {
            if (handle_)
            {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    task(const task&) = delete;
    task& operator=(const task&) = delete;

    /// Destroys an owned frame without running an unstarted body.
    ~task()
    {
        if (handle_)
        {
            handle_.destroy();
        }
    }

    /// Empties the task and returns its raw handle. The caller must resume, complete and destroy it exactly once.
    handle_type release() noexcept
    {
        return std::exchange(handle_, {});
    }

    /// Owns the frame for one co_await and destroys it afterwards.
    struct awaiter
    {
        handle_type handle;

        awaiter(handle_type h) noexcept
            : handle(h)
        {
        }

        awaiter(awaiter&& other) noexcept
            : handle(std::exchange(other.handle, {}))
        {
        }

        awaiter(const awaiter&) = delete;
        awaiter& operator=(const awaiter&) = delete;
        awaiter& operator=(awaiter&&) = delete;

        ~awaiter()
        {
            if (handle)
            {
                handle.destroy();
            }
        }

        /// True for an empty or finished frame.
        bool await_ready() const noexcept
        {
            return !handle || handle.done();
        }

        /// Passes the parent's token on (unless the child has a bound one), throws std::system_error if it is
        /// already cancelled, and transfers control to the child.
        template <class P>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<P> parent)
        {
            auto& promise = handle.promise();
            if (!promise.cancellation_bound)
            {
                if constexpr (requires { parent.promise().cancellation; })
                {
                    promise.cancellation = parent.promise().cancellation;
                }
            }
            promise.cancellation.throw_if_cancelled();
            handle.promise().continuation = parent;
            return handle;
        }

        /// Rethrows the child's exception, throws std::system_error if its token was cancelled by the time it
        /// finished. Throws std::logic_error for an empty task.
        void await_resume()
        {
            if (!handle)
            {
                throw std::logic_error("cannot await an empty task");
            }
            if (handle.promise().exception)
            {
                std::rethrow_exception(handle.promise().exception);
            }
            handle.promise().cancellation.throw_if_cancelled();
        }
    };

    /// Only an rvalue can be awaited: write `co_await std::move(work)`.
    awaiter operator co_await() && noexcept
    {
        return awaiter{release()};
    }

private:
    friend struct task_access;

    explicit task(handle_type handle) noexcept
        : handle_(handle)
    {
    }

    handle_type handle_{};
};

/// Implementation support for boundaries and groups; use with_cancellation or shield instead.
struct task_access
{
    /// Gives an unstarted task its own token, which it keeps instead of inheriting the awaiting parent's.
    template <class T>
    static task<T> bind(task<T> work, cancellation_token token)
    {
        if (work.handle_)
        {
            work.handle_.promise().cancellation = std::move(token);
            work.handle_.promise().cancellation_bound = true;
        }
        return work;
    }
};

namespace this_task
{
/// Awaiter of this_task::cancellation(); never suspends.
struct cancellation_awaiter
{
    pace::cancellation_token value;

    bool await_ready() const noexcept
    {
        return false;
    }

    template <class P>
    bool await_suspend(std::coroutine_handle<P> h) noexcept
    {
        if constexpr (requires { h.promise().cancellation; })
        {
            value = h.promise().cancellation;
        }
        return false;
    }

    pace::cancellation_token await_resume() const noexcept
    {
        return value;
    }
};

/// `co_await this_task::cancellation()` returns the current task's token without throwing.
inline cancellation_awaiter cancellation() noexcept
{
    return {};
}
} // namespace this_task
} // namespace pace
