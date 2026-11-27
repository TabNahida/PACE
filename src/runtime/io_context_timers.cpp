// Callback timers and timer awaiters.
#include "runtime/io_context_state.hpp"

namespace pace
{
io_context::timer_handle::~timer_handle()
{
    cancel();
}

io_context::timer_handle::timer_handle(timer_handle&& other) noexcept
    : timers_(std::move(other.timers_))
    , deadline_(other.deadline_)
    , id_(std::exchange(other.id_, 0))
{
}

io_context::timer_handle& io_context::timer_handle::operator=(timer_handle&& other) noexcept
{
    if (this != &other)
    {
        cancel();
        timers_ = std::move(other.timers_);
        deadline_ = other.deadline_;
        id_ = std::exchange(other.id_, 0);
    }
    return *this;
}

void io_context::timer_handle::cancel() noexcept
{
    const auto id = std::exchange(id_, 0);
    if (id)
    {
        if (auto timers = timers_.lock())
        {
            // Captures can cancel other timers, so destroy outside the map.
            auto node = timers->callbacks.extract({deadline_, id});
        }
    }
    timers_.reset();
}

io_context::timer_handle io_context::call_at(
    std::chrono::steady_clock::time_point deadline,
    std::function<void()> callback
)
{
    if (impl_->running.load(std::memory_order_acquire))
    {
        check_affinity(this);
    }
    if (stopped())
    {
        throw std::system_error(canceled());
    }
    if (!callback)
    {
        throw std::invalid_argument("empty timer callback");
    }
    if (!impl_->callbacks)
    {
        impl_->callbacks = std::make_shared<callback_timers>();
    }
    auto& timers = *impl_->callbacks;
    if (timers.next_id == UINT64_MAX)
    {
        throw std::overflow_error("timer identity exhausted");
    }
    ++timers.next_id;
    timer_handle handle;
    handle.deadline_ = deadline;
    handle.id_ = timers.next_id;
    handle.timers_ = impl_->callbacks;
    timers.callbacks.emplace(callback_timers::key{deadline, handle.id_}, std::move(callback));
    return handle;
}

bool io_context::timer_awaiter::await_ready() noexcept
{
    if (context.stopped())
    {
        error = canceled();
        return true;
    }
    return false; // Even zero-duration sleeps yield, so they cannot starve shutdown.
}

void io_context::timer_awaiter::await_resume()
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

bool io_context::arm(timer_awaiter& operation, std::coroutine_handle<> handle)
{
    check_affinity(this);
    if (stopped())
    {
        operation.error = canceled();
        return false;
    }
    operation.continuation = handle;
    impl_->timers.push(&operation);
    return true;
}
} // namespace pace
