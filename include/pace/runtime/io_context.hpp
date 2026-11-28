#pragma once

#include <pace/runtime/io_backend.hpp>
#include <pace/runtime/platform.hpp>
#include <pace/runtime/task.hpp>
#include <pace/runtime/unique_function.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <system_error>
#include <utility>

namespace pace
{
struct scheduler_access;
struct remote_access;

/// One reactor run by one thread. spawn, post, stop and stopped are thread-safe; sockets, timers and waits are used
/// only from the thread running the context. Single-use: it cannot be restarted.
class io_context
{
    struct callback_timers;
    struct cancellation_records;

public:
    /// Creates the poller. Throws std::system_error if it cannot be created or `selected` is unavailable, and
    /// std::invalid_argument for an unknown PACE_IO_BACKEND.
    explicit io_context(io_backend selected = io_backend::automatic);
    /// The backend in use; never automatic.
    [[nodiscard]] io_backend backend() const noexcept;
    /// Stops, cancels waits and destroys queued work, owned roots and services. run() must have returned.
    ~io_context();
    io_context(const io_context&) = delete;
    io_context& operator=(const io_context&) = delete;

    /// Returns the service stored under `key.get()`, creating it with `create` the first time. Keys and values live
    /// until the context is destroyed. Call on the running context's thread (std::logic_error otherwise); throws
    /// std::invalid_argument for a null key, empty `create` or null result, operation_canceled once stopped.
    std::shared_ptr<void> local_state(
        std::shared_ptr<const void> key,
        const std::function<std::shared_ptr<void>()>& create
    );
    /// Borrows the service stored under `key`, or nullptr. Valid until the context is destroyed. Call on the running
    /// context's thread; throws operation_canceled once stopped.
    void* find_local_state(const void* key);
    /// Takes ownership of a root task and schedules it; never runs it inline. Empty tasks are ignored. Thread-safe.
    /// Throws std::system_error(operation_canceled) once stopped.
    void spawn(task<void> work);
    /// Queues `work` to run on this context, never inline. Thread-safe. Throws std::invalid_argument for an empty
    /// function and std::system_error(operation_canceled) once stopped.
    void post(unique_function<void()> work);
    /// Runs on the calling thread until stop() and the drain finish, then rethrows the first unhandled error of a
    /// root, callback or the poller. Throws std::logic_error for a nested, concurrent or repeated run.
    void run();
    /// Requests a permanent stop: pending waits resume with operation_canceled. Thread-safe, idempotent, no wait.
    void stop() noexcept;
    /// Whether stop was requested. Thread-safe.
    bool stopped() const noexcept;
    /// The context running on the calling thread, or nullptr.
    static io_context* current() noexcept;
    /// cancel(fd), then drops the backend registration. Call immediately before closing a raw socket or any other
    /// awaited descriptor yourself.
    /// Must run on the owning thread, or while no thread runs the context.
    void release_socket(native_socket_type fd) noexcept;
    /// Resumes the descriptor's pending waits with operation_canceled; the descriptor stays open and usable.
    /// Must run on the owning thread, or while no thread runs the context.
    void cancel(native_socket_type fd) noexcept;

    /// Owns one callback timer: it stays armed while the handle exists. cancel(), destruction and move assignment
    /// release the callback at once. Use on the owning thread, or while no thread runs the context. A handle may
    /// outlive its io_context.
    class timer_handle
    {
    public:
        timer_handle() noexcept = default;
        ~timer_handle();
        timer_handle(timer_handle&&) noexcept;
        timer_handle& operator=(timer_handle&&) noexcept;
        timer_handle(const timer_handle&) = delete;
        timer_handle& operator=(const timer_handle&) = delete;
        /// Disarms the timer or registration and destroys its callback now; does nothing if already released.
        void cancel() noexcept;

    private:
        friend class io_context;
        std::weak_ptr<callback_timers> timers_;
        std::chrono::steady_clock::time_point deadline_{};
        std::uint64_t id_ = 0;
    };

    /// Runs `callback` once on this context at `deadline`; never inline. Register on the owning thread or before
    /// run(). Throws std::invalid_argument for an empty callback, operation_canceled once stopped. Stop discards
    /// pending callbacks.
    [[nodiscard]] timer_handle call_at(
        std::chrono::steady_clock::time_point deadline,
        unique_function<void()> callback
    );

    /// call_at(now + duration, callback), with the deadline computed like sleep_for.
    template <class Rep, class Period>
    [[nodiscard]] timer_handle call_after(std::chrono::duration<Rep, Period> duration, unique_function<void()> callback)
    {
        return call_at(sleep_for(duration).deadline, std::move(callback));
    }

    /// Owns one on_cancel registration; cancel(), destruction and move assignment unregister it. Use on the owning
    /// thread, or while no thread runs the context. A handle may outlive its io_context.
    class cancellation_handle
    {
    public:
        cancellation_handle() noexcept = default;
        ~cancellation_handle();
        cancellation_handle(cancellation_handle&&) noexcept;
        cancellation_handle& operator=(cancellation_handle&&) noexcept;
        cancellation_handle(const cancellation_handle&) = delete;
        cancellation_handle& operator=(const cancellation_handle&) = delete;
        /// Disarms the timer or registration and destroys its callback now; does nothing if already released.
        void cancel() noexcept;

    private:
        friend class io_context;
        std::weak_ptr<cancellation_records> records_;
        std::uint64_t id_ = 0;
    };

    /// Runs `callback` on this context with the token's error when `token` is cancelled (by a request, not merely a
    /// passed deadline) or when the context stops. An empty token registers nothing. Otherwise call on the owning
    /// thread; throws operation_canceled once stopped, std::invalid_argument for an empty callback, and the token's
    /// error if it is already cancelled.
    [[nodiscard]] cancellation_handle on_cancel(cancellation_token, unique_function<void(std::error_code)>);

    /// A readiness wait: create with wait_*, await once, immediately, on the owning context; do not move or modify
    /// it while suspended. Fails with the task token's error, operation_canceled on stop or cancel(fd), or
    /// bad_file_descriptor for invalid_socket.
    struct io_awaiter
    {
        io_context& context;
        native_socket_type fd;
        bool writable;
        std::error_code error;
        std::coroutine_handle<> continuation;
        bool either = false;
        cancellation_token token{};
        cancellation_handle registration{};
        bool await_ready() noexcept;

        template <class P>
        bool await_suspend(std::coroutine_handle<P> handle)
        {
            if constexpr (requires { handle.promise().cancellation; })
            {
                token = handle.promise().cancellation;
            }
            if (token.stop_possible())
            {
                if (auto e = token.error())
                {
                    error = e;
                    return false;
                }
                registration = context.on_cancel(token, [this](std::error_code e) { context.cancel_wait(*this, e); });
            }
            return context.arm(*this, handle);
        }

        std::error_code resume_error() noexcept;
        void await_resume();
    };

    /// Suspends until `fd` is readable, at end of stream or in error. Transfers no bytes; retry the nonblocking
    /// call afterwards. One pending wait per descriptor and direction (std::logic_error otherwise). On POSIX `fd`
    /// may be any descriptor the backend can poll (socket, pipe, terminal, TUN device, eventfd); a descriptor the
    /// poller rejects throws std::system_error. On Windows it must be a Winsock socket; see wait_handle.
    io_awaiter wait_readable(native_socket_type fd) noexcept
    {
        return {*this, fd, false, {}, {}};
    }

    /// Suspends until `fd` is writable or in error. One pending wait per descriptor and direction.
    io_awaiter wait_writable(native_socket_type fd) noexcept
    {
        return {*this, fd, true, {}, {}};
    }

    /// Suspends until either direction can make progress; resumes once. Occupies both directions.
    io_awaiter wait_ready(native_socket_type fd) noexcept
    {
        return {*this, fd, false, {}, {}, true};
    }

#ifdef _WIN32
    /// A wait for a Windows kernel object, from wait_handle: await once, immediately, on the owning context; do not
    /// move or modify it while suspended. Fails with the task token's error, operation_canceled on stop, or
    /// bad_file_descriptor for a null or INVALID_HANDLE_VALUE handle.
    struct handle_awaiter
    {
        io_context& context;
        HANDLE object;
        std::error_code error;
        std::coroutine_handle<> continuation;
        cancellation_token token{};
        cancellation_handle registration{};
        /// The thread-pool wait (a PTP_WAIT) and its reactor key while suspended.
        void* wait = nullptr;
        std::uint64_t id = 0;
        bool await_ready() noexcept;

        template <class P>
        bool await_suspend(std::coroutine_handle<P> waiting)
        {
            if constexpr (requires { waiting.promise().cancellation; })
            {
                token = waiting.promise().cancellation;
            }
            if (token.stop_possible())
            {
                if (auto e = token.error())
                {
                    error = e;
                    return false;
                }
                registration = context.on_cancel(token, [this](std::error_code e) { context.cancel_wait(*this, e); });
            }
            return context.arm(*this, waiting);
        }

        void await_resume();
    };

    /// Suspends until `object` (an event, semaphore, process, thread, waitable timer or similar kernel object; not a
    /// mutex) is signaled, consuming an auto-reset signal like WaitForSingleObject. A thread-pool wait observes the
    /// handle and wakes the reactor; any number of waits may target one handle. The handle must stay open until the
    /// wait ends. Windows only: on POSIX, wait_readable on a descriptor (eventfd, pidfd, pipe) is the equivalent.
    handle_awaiter wait_handle(HANDLE object) noexcept
    {
        return {*this, object, {}, {}};
    }
#endif

    /// A timer wait from sleep_for: await once, immediately, on the owning context. Fails with the task token's
    /// error or operation_canceled on stop.
    struct timer_awaiter
    {
        io_context& context;
        std::chrono::steady_clock::time_point deadline;
        std::error_code error;
        std::coroutine_handle<> continuation;
        cancellation_token token{};
        cancellation_handle registration{};
        /// Position in the owning reactor's deadline heap while suspended.
        std::size_t heap_index = static_cast<std::size_t>(-1);
        bool await_ready() noexcept;

        template <class P>
        bool await_suspend(std::coroutine_handle<P> handle)
        {
            if constexpr (requires { handle.promise().cancellation; })
            {
                token = handle.promise().cancellation;
            }
            if (token.stop_possible())
            {
                if (auto e = token.error())
                {
                    error = e;
                    return false;
                }
                registration = context.on_cancel(token, [this](std::error_code e) { context.cancel_wait(*this, e); });
            }
            return context.arm(*this, handle);
        }

        void await_resume();
    };

    /// Suspends for `duration`, computed when called (not when awaited). Durations <= 0 still suspend one turn;
    /// durations too large for the clock saturate. Timers never fire early and may fire late.
    template <class Rep, class Period>
    timer_awaiter sleep_for(std::chrono::duration<Rep, Period> duration) noexcept
    {
        using clock = std::chrono::steady_clock;
        using ticks = std::chrono::duration<long double, clock::period>;
        const auto now = clock::now();
        const auto requested = ticks(duration).count();
        const auto maximum = static_cast<long double>(clock::time_point::max().time_since_epoch().count());
        const auto absolute = static_cast<long double>(now.time_since_epoch().count()) + requested;
        auto deadline = now;
        if (requested > 0)
        {
            deadline = absolute >= maximum ? clock::time_point::max()
                                           : clock::time_point(clock::duration(static_cast<clock::rep>(absolute)));
        }
        return {*this, deadline, {}, {}};
    }

    /// Requeues the coroutine behind other ready work. Throws on resume if the token is cancelled or the context
    /// stopped.
    struct yield_awaiter
    {
        io_context& context;

        bool await_ready() const noexcept
        {
            return false;
        }

        cancellation_token token{};

        template <class P>
        void await_suspend(std::coroutine_handle<P> handle)
        {
            if constexpr (requires { handle.promise().cancellation; })
            {
                token = handle.promise().cancellation;
            }
            context.schedule(handle);
        }

        void await_resume();
    };

    /// Lets other ready coroutines, callbacks and I/O run first. Await on the owning context.
    yield_awaiter yield() noexcept
    {
        return {*this};
    }

private:
    friend struct scheduler_access;
    friend struct remote_access;
    void cancel_wait(io_awaiter&, std::error_code);
    void cancel_wait(timer_awaiter&, std::error_code);
    struct implementation;
    std::unique_ptr<implementation> impl_;
    bool arm(io_awaiter&, std::coroutine_handle<>);
    bool arm(timer_awaiter&, std::coroutine_handle<>);
#ifdef _WIN32
    void cancel_wait(handle_awaiter&, std::error_code);
    bool arm(handle_awaiter&, std::coroutine_handle<>);
#endif
    void schedule(std::coroutine_handle<>);
    static void completed(void*, std::coroutine_handle<>) noexcept;
};

/// Implementation support for task groups: queues a suspended coroutine on `io`. Call on the thread running `io`.
struct scheduler_access
{
    static void schedule(io_context& io, std::coroutine_handle<> handle)
    {
        io.schedule(handle);
    }
};

/// Implementation support for channel, snapshot and on(): a reference to a context that stays safe to use from any
/// thread after the context has stopped or been destroyed, and the hooks built on it. Not part of the API.
struct remote_access
{
    /// Shared between a context and everyone that holds its anchor; defined in the library.
    struct anchor;

    /// Notified once, on the context's thread, when the context starts its shutdown; or, for a context that never
    /// shut down, while it is destroyed. Held weakly: destroying the owner of a listener unregisters it.
    struct listener
    {
        void (*notify)(listener&) noexcept = nullptr;
    };

    /// The anchor of `io`. Thread-safe.
    static std::shared_ptr<anchor> anchor_of(io_context& io);

    /// The anchor of the context running on this thread, or nullptr. Comparing anchors, unlike context addresses,
    /// cannot mistake a new context for a destroyed one: a held anchor is never reused.
    static anchor* current() noexcept;

    /// Queues `work` on the anchored context from any thread. Unlike io_context::post it is still accepted after
    /// stop() (unless `while_stopping` is false), so a coroutine suspended there can be resumed while the context
    /// drains. Accepted work runs if run() is running or yet to run and has not finished its drain; otherwise it is
    /// destroyed unrun with the context, like the suspended frames it would resume. Returns false, without keeping
    /// `work`, once the context is destroyed.
    static bool post(anchor& target, unique_function<void()> work, bool while_stopping = true);

    /// Resumes `handle` on the anchored context: queued directly when called on its thread, otherwise posted.
    /// Returns false when the context can no longer run it.
    static bool resume(anchor& target, std::coroutine_handle<> handle);

    /// Registers `subscriber`. Returns false, registering nothing, once the context has shut down. Thread-safe.
    static bool listen(anchor& target, std::weak_ptr<listener> subscriber);

    /// io_context::on_cancel(token, callback) for a token that can be cancelled; for an empty token, a registration
    /// that only the context's stop triggers. Call on the thread running `io`.
    static io_context::cancellation_handle on_cancel_or_stop(
        io_context& io,
        const cancellation_token& token,
        unique_function<void(std::error_code)> callback
    );
};
} // namespace pace
