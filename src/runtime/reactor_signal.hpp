#pragma once
#include <pace/runtime/io_context.hpp>

#include <chrono>
#include <coroutine>
#include <utility>

namespace pace
{
// Context-affine broadcast for protocol progress. Waiters live in coroutine
// frames; no list-node allocation and no ownership cycle. The enclosing
// operation must keep the signal alive. A transport pump notifies on shutdown.
class reactor_signal
{
public:
    using clock = std::chrono::steady_clock;

    struct awaiter
    {
        reactor_signal& signal;
        clock::time_point deadline;
        awaiter* previous = nullptr;
        awaiter* next = nullptr;
        std::coroutine_handle<> continuation{};
        io_context::timer_handle timer;
        io_context::cancellation_handle registration;
        std::error_code error;
        bool linked = false;

        awaiter(reactor_signal& owner, clock::time_point until)
            : signal(owner)
            , deadline(until)
        {
        }

        awaiter(const awaiter&) = delete;
        awaiter& operator=(const awaiter&) = delete;

        ~awaiter()
        {
            unlink();
        }

        void unlink() noexcept
        {
            if (!linked)
            {
                return;
            }
            if (previous)
            {
                previous->next = next;
            }
            else
            {
                signal.first_ = next;
            }
            if (next)
            {
                next->previous = previous;
            }
            linked = false;
        }

        bool await_ready() const noexcept
        {
            return signal.io_.stopped();
        }

        template <class P>
        void await_suspend(std::coroutine_handle<P> handle)
        {
            if constexpr (requires { handle.promise().cancellation; })
            {
                registration = signal.io_.on_cancel(
                    handle.promise().cancellation,
                    [this](std::error_code reason)
                    {
                        error = reason;
                        resume();
                    }
                );
            }
            continuation = handle;
            next = signal.first_;
            if (next)
            {
                next->previous = this;
            }
            signal.first_ = this;
            linked = true;
            try
            {
                if (deadline != clock::time_point::max())
                {
                    timer = signal.io_.call_at(deadline, [this] { resume(); });
                }
            }
            catch (...)
            {
                unlink();
                throw;
            }
        }

        void resume()
        {
            if (!linked)
            {
                return;
            }
            unlink();
            registration.cancel();
            timer.cancel();
            // Running reactors queue continuations rather than reentering user
            // code inside a protocol callback. Outside run(), only teardown can
            // have pending waiters and cancellation must still unwind them.
            if (io_context::current() == &signal.io_)
            {
                signal.io_.yield().await_suspend(continuation);
            }
            else
            {
                continuation.resume();
            }
        }

        void await_resume() const
        {
            if (error)
            {
                throw std::system_error(error);
            }
            if (signal.io_.stopped())
            {
                throw std::system_error(std::make_error_code(std::errc::operation_canceled));
            }
        }
    };

    explicit reactor_signal(io_context& io)
        : io_(io)
    {
    }

    reactor_signal(const reactor_signal&) = delete;
    reactor_signal& operator=(const reactor_signal&) = delete;
    ~reactor_signal() = default;

    awaiter wait(clock::time_point deadline = clock::time_point::max())
    {
        return {*this, deadline};
    }

    void notify()
    {
        while (first_)
        {
            first_->resume();
        }
    }

private:
    io_context& io_;
    awaiter* first_ = nullptr;
};
} // namespace pace
