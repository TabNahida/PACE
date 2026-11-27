// remote_access: the context anchor behind channel, snapshot and on().
#include "runtime/io_context_state.hpp"

namespace pace
{
// Listeners run without the anchor lock: they post to other contexts and may take their own locks.
void io_context::implementation::notify_shutdown() noexcept
{
    std::vector<std::weak_ptr<remote_access::listener>> subscribers;
    {
        std::scoped_lock lock(anchor->mutex);
        if (anchor->shut_down)
        {
            return;
        }
        anchor->shut_down = true;
        subscribers.swap(anchor->listeners);
    }
    for (const auto& subscriber : subscribers)
    {
        if (auto target = subscriber.lock())
        {
            target->notify(*target);
        }
    }
}

std::shared_ptr<remote_access::anchor> remote_access::anchor_of(io_context& io)
{
    return io.impl_->anchor;
}

bool remote_access::post(anchor& target, std::function<void()> work, bool while_stopping)
{
    // Holding the anchor lock keeps the context alive: ~io_context takes it before tearing anything down.
    std::scoped_lock remote(target.mutex);
    if (!target.context || (!while_stopping && target.context->stopped()))
    {
        return false;
    }
    auto& p = *target.context->impl_;
    bool notify;
    {
        std::scoped_lock lock(p.inbox_mutex);
        p.incoming_posts.push_back(std::move(work));
        notify = p.mark_inbox();
    }
    if (notify)
    {
        p.poller.wake();
    }
    return true;
}

remote_access::anchor* remote_access::current() noexcept
{
    return active_context ? active_context->impl_->anchor.get() : nullptr;
}

bool remote_access::resume(anchor& target, std::coroutine_handle<> handle)
{
    if (current() == &target)
    {
        active_context->schedule(handle);
        return true;
    }
    return post(target, [handle] { handle.resume(); });
}

bool remote_access::listen(anchor& target, std::weak_ptr<listener> subscriber)
{
    std::scoped_lock lock(target.mutex);
    if (target.shut_down)
    {
        return false;
    }
    auto& list = target.listeners;
    if (list.size() == list.capacity())
    {
        // Listeners are held weakly; drop the expired ones before growing.
        std::erase_if(list, [](const auto& entry) { return entry.expired(); });
    }
    list.push_back(std::move(subscriber));
    return true;
}

io_context::cancellation_handle remote_access::on_cancel_or_stop(
    io_context& io,
    const cancellation_token& token,
    std::function<void(std::error_code)> callback
)
{
    if (token.stop_possible())
    {
        return io.on_cancel(token, std::move(callback));
    }
    check_affinity(&io);
    auto& p = *io.impl_;
    if (!p.stop_only)
    {
        p.stop_only.emplace();
    }
    // Nothing ever cancels this token, so the registration fires only at the context's stop.
    return io.on_cancel(p.stop_only->token(), std::move(callback));
}
} // namespace pace
