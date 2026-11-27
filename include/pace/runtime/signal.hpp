#pragma once

#include <pace/runtime/io_context.hpp>
#include <pace/runtime/task.hpp>

#include <array>
#include <coroutine>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <system_error>

namespace pace
{
/// A process-level event a program can await: a POSIX signal, or its Windows console or service equivalent.
enum class process_signal
{
    hangup,    ///< SIGHUP, conventionally "reload configuration". Windows: only deliver_signal.
    interrupt, ///< SIGINT; Windows CTRL_C_EVENT and CTRL_BREAK_EVENT.
    terminate, ///< SIGTERM; Windows CTRL_CLOSE_EVENT, CTRL_LOGOFF_EVENT and CTRL_SHUTDOWN_EVENT.
    user1,     ///< SIGUSR1. Windows: only deliver_signal.
    user2      ///< SIGUSR2. Windows: only deliver_signal.
};

/// The conventional name of `value` ("SIGHUP", "SIGINT", "SIGTERM", "SIGUSR1", "SIGUSR2"), on every platform.
const char* signal_name(process_signal value) noexcept;

/// A subscription to process signals for one io_context. While at least one signal_set in the process subscribes to
/// a signal, PACE owns its disposition (POSIX sigaction, Windows console control handler); the previous disposition
/// is restored when the last one is destroyed. Every signal_set sees every delivery of its signals that happens
/// after its construction, in every context; deliveries that arrive while no wait is pending are counted and
/// reported by later waits. Construction and destruction may happen on any thread (also before run()); wait() runs
/// on the thread running the context. Neither copyable nor movable.
class signal_set
{
public:
    /// The result of wait(): await once, immediately, on the owning context; do not move or modify it while
    /// suspended. Resumes with the signal delivered; throws the task token's error, or operation_canceled on stop.
    struct awaiter
    {
        signal_set& set;
        std::error_code error;
        process_signal value{};
        std::coroutine_handle<> continuation;
        cancellation_token token{};
        io_context::cancellation_handle registration{};
        /// The context's list of pending signal waits.
        awaiter* previous = nullptr;
        awaiter* next = nullptr;
        bool linked = false;

        explicit awaiter(signal_set& owner) noexcept
            : set(owner)
        {
        }

        awaiter(const awaiter&) = delete;
        awaiter& operator=(const awaiter&) = delete;
        /// Unlinks a wait whose coroutine is destroyed while suspended.
        ~awaiter();
        /// True when a delivery is already pending (it is consumed) or the context is stopped.
        bool await_ready();

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
                registration = set.io_.on_cancel(token, [this](std::error_code e) { set.cancel_wait(*this, e); });
            }
            return set.arm(*this, handle);
        }

        process_signal await_resume();
    };

    /// Subscribes `io` to `signals` (duplicates are ignored). Throws std::invalid_argument for an empty list or an
    /// unknown value, std::system_error if a disposition cannot be installed or every notifier slot is taken.
    signal_set(io_context& io, std::initializer_list<process_signal> signals);
    /// Unsubscribes; restores a signal's previous disposition when this was its last subscription. No wait may be
    /// pending unless its coroutine is being destroyed.
    ~signal_set();
    signal_set(const signal_set&) = delete;
    signal_set& operator=(const signal_set&) = delete;

    /// Suspends until one of the subscribed signals is delivered and returns it; a pending delivery completes it at
    /// once. Several pending signals are reported one per wait, in enumeration order. One pending wait per set
    /// (std::logic_error otherwise). Await on the context's thread.
    [[nodiscard]] awaiter wait() noexcept
    {
        return awaiter(*this);
    }

    /// Whether `value` is one of the subscribed signals.
    [[nodiscard]] bool contains(process_signal value) const noexcept;

    /// The context this set belongs to.
    [[nodiscard]] io_context& context() const noexcept
    {
        return io_;
    }

private:
    static constexpr std::size_t signal_count = 5;
    io_context& io_;
    unsigned mask_ = 0;
    /// Per signal, the process-wide delivery count already reported (or present at construction); wraps.
    std::array<std::uint32_t, signal_count> seen_{};
    /// The context's signal service, acquired by the first wait.
    std::shared_ptr<void> service_;
    awaiter* waiter_ = nullptr;

    bool take(process_signal& value) noexcept;
    bool arm(awaiter&, std::coroutine_handle<>);
    void cancel_wait(awaiter&, std::error_code) noexcept;
    friend struct signal_access;
};

/// Subscribes to `signals` now, then returns a task that waits for the first delivery and returns it. The
/// subscription lives in the task, so a signal raised after this call and before the task is awaited is not lost.
/// Throws like signal_set's constructor; the task throws like signal_set::wait().
task<process_signal> wait_for_signal(io_context& io, std::initializer_list<process_signal> signals);
/// wait_for_signal(this_context(), signals). Throws std::logic_error if no context runs on this thread.
task<process_signal> wait_for_signal(std::initializer_list<process_signal> signals);

/// Delivers `value` to every subscribed signal_set as if the operating system had raised it, without running any
/// default action. Thread-safe and, on POSIX, async-signal-safe. Use it from a Windows service control handler for
/// SERVICE_CONTROL_STOP, or to trigger a reload from inside the program.
void deliver_signal(process_signal value) noexcept;
} // namespace pace
