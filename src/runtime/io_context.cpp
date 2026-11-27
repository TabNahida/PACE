#include "runtime/io_context_state.hpp"

namespace pace
{
constinit thread_local io_context* active_context = nullptr;

io_context::io_context(io_backend selected)
    : impl_(std::make_unique<implementation>(selected))
{
}

io_backend io_context::backend() const noexcept
{
    return impl_->poller.backend();
}

io_context::~io_context()
{
    // Caller must join run() before destruction. Destruct user objects while the
    // entire implementation is alive, and reject reentrant submissions.
    stop();
    impl_->cancel_all(false);
    impl_->incoming_tasks.clear();
    impl_->incoming_posts.clear();
    impl_->posts.clear();
    for (auto root : impl_->roots)
    {
        root.destroy();
    }
    impl_->roots.clear();
    impl_->ready.clear();
    impl_->services.clear();
}

io_context* io_context::current() noexcept
{
    return active_context;
}

bool io_context::stopped() const noexcept
{
    return impl_->stopping.load(std::memory_order_acquire);
}

void io_context::stop() noexcept
{
    if (!impl_->stopping.exchange(true, std::memory_order_acq_rel))
    {
        impl_->poller.wake();
    }
}

std::shared_ptr<void> io_context::local_state(
    std::shared_ptr<const void> key,
    const std::function<std::shared_ptr<void>()>& create
)
{
    check_affinity(this);
    if (!key || !create)
    {
        throw std::invalid_argument("invalid context service");
    }
    if (stopped())
    {
        throw std::system_error(canceled());
    }
    auto it = impl_->services.find(key.get());
    if (it != impl_->services.end())
    {
        return it->second.value;
    }
    auto value = create();
    if (!value)
    {
        throw std::invalid_argument("empty context service");
    }
    const auto address = key.get();
    impl_->services.emplace(address, implementation::service{std::move(key), value});
    return value;
}

void* io_context::find_local_state(const void* key)
{
    check_affinity(this);
    if (stopped())
    {
        throw std::system_error(canceled());
    }
    const auto it = impl_->services.find(key);
    return it == impl_->services.end() ? nullptr : it->second.value.get();
}

void io_context::spawn(task<void> work)
{
    auto& p = *impl_;
    if (current() == this)
    {
        if (stopped())
        {
            throw std::system_error(canceled());
        }
        p.adopt_root(*this, work);
        return;
    }
    bool notify;
    {
        std::lock_guard lock(p.inbox_mutex);
        if (stopped())
        {
            throw std::system_error(canceled());
        }
        p.incoming_tasks.push_back(std::move(work));
        notify = p.mark_inbox();
    }
    if (notify)
    {
        p.poller.wake();
    }
}

void io_context::post(std::function<void()> work)
{
    if (!work)
    {
        throw std::invalid_argument("empty posted function");
    }
    auto& p = *impl_;
    if (current() == this)
    {
        if (stopped())
        {
            throw std::system_error(canceled());
        }
        p.posts.push_back(std::move(work));
        return;
    }
    bool notify;
    {
        std::lock_guard lock(p.inbox_mutex);
        if (stopped())
        {
            throw std::system_error(canceled());
        }
        p.incoming_posts.push_back(std::move(work));
        notify = p.mark_inbox();
    }
    if (notify)
    {
        p.poller.wake();
    }
}

void io_context::completed(void* context, std::coroutine_handle<> handle) noexcept
{
    auto& p = *static_cast<io_context*>(context)->impl_;
    auto h = task<void>::handle_type::from_address(handle.address());
    h.promise().completed_next = p.completed_head;
    p.completed_head = handle;
}

void io_context::schedule(std::coroutine_handle<> handle)
{
    check_affinity(this);
    impl_->ready.push_back(handle);
}

void io_context::yield_awaiter::await_resume()
{
    token.throw_if_cancelled();
    if (context.stopped())
    {
        throw std::system_error(canceled());
    }
}

void io_context::run()
{
    auto& p = *impl_;
    if (active_context)
    {
        throw std::logic_error("nested io_context::run is not supported");
    }
    if (p.running.exchange(true))
    {
        throw std::logic_error("io_context already running");
    }

    struct reset_guard
    {
        implementation& p;

        ~reset_guard()
        {
            active_context = nullptr;
            p.running.store(false);
        }
    } guard{p};

    if (p.has_run)
    {
        throw std::logic_error("io_context is single-use");
    }
    p.has_run = true;
    active_context = this;
    profiling::scope run_timing(profiling::stage::reactor_run);
    auto record_error = [&](std::exception_ptr error)
    {
        try
        {
            if (error)
            {
                std::rethrow_exception(error);
            }
        }
        catch (const std::system_error& e)
        {
            if (stopped() && e.code() == std::errc::operation_canceled)
            {
                return;
            }
            if (!p.first_error)
            {
                p.first_error = error;
            }
            stop();
        }
        catch (...)
        {
            if (!p.first_error)
            {
                p.first_error = error;
            }
            stop();
        }
    };
    std::array<pace::poll_event, 256> events{};
    // Reuse the empty batches between drains. Reconstructing two deques on
    // every producer wake allocates their maps/blocks even for a single post.
    std::deque<task<void>> new_tasks;
    std::deque<std::function<void()>> new_posts;
    bool inbox_buffered = false;
    for (;;)
    {
        profiling::scope inbox_timing(profiling::stage::inbox);
        bool inbox_deferred = p.inbox_pending.load(std::memory_order_acquire);
        if (inbox_deferred || inbox_buffered)
        {
            if (inbox_deferred)
            {
                const bool take_tasks = new_tasks.empty();
                const bool take_posts = new_posts.empty();
                if (take_tasks || take_posts)
                {
                    std::lock_guard lock(p.inbox_mutex);
                    if (take_tasks)
                    {
                        new_tasks.swap(p.incoming_tasks);
                    }
                    if (take_posts)
                    {
                        new_posts.swap(p.incoming_posts);
                    }
                    // Leave new arrivals in a category queued until its local batch drains.
                    inbox_deferred =
                        (!take_tasks && !p.incoming_tasks.empty()) || (!take_posts && !p.incoming_posts.empty());
                    p.inbox_pending.store(inbox_deferred, std::memory_order_relaxed);
                }
            }
            if (!new_tasks.empty())
            {
                p.promote_batch(new_tasks, [&](auto& work) { p.adopt_root(*this, work); });
            }
            if (!new_posts.empty())
            {
                p.promote_batch(new_posts, [&](auto& work) { p.posts.push_back(std::move(work)); });
            }
            inbox_buffered = !new_tasks.empty() || !new_posts.empty();
        }
        inbox_timing.close();
        if (p.cancellations && p.cancellations->pending.load(std::memory_order_acquire))
        {
            record_error(p.cancellations->dispatch());
        }
        if (stopped() && !p.cancellation_started)
        {
            p.cancel_all(true);
        }
        profiling::scope post_timing(profiling::stage::post_dispatch);
        for (std::size_t i = 0, count = std::min(p.posts.size(), dispatch_limit); i < count; ++i)
        {
            auto work = std::move(p.posts.front());
            p.posts.pop_front();
            try
            {
                work();
            }
            catch (...)
            {
                record_error(std::current_exception());
            }
        }
        post_timing.close();
        profiling::scope ready_timing(profiling::stage::coroutine_execution);
        for (std::size_t i = 0, count = std::min(p.ready.size(), dispatch_limit); i < count; ++i)
        {
            auto handle = p.ready.front();
            p.ready.pop_front();
            handle.resume();
        }
        ready_timing.close();
        profiling::scope reap_timing(profiling::stage::root_reaping);
        while (p.completed_head)
        {
            auto h = task<void>::handle_type::from_address(p.completed_head.address());
            p.completed_head = h.promise().completed_next;
            record_error(h.promise().exception);
            const auto slot = h.promise().root_index;
            p.roots[slot] = p.roots.back();
            p.roots[slot].promise().root_index = slot;
            p.roots.pop_back();
            h.destroy();
        }
        reap_timing.close();
        if (stopped())
        {
            if (!p.cancellation_started)
            {
                p.cancel_all(true);
            }
            bool inbox_empty;
            {
                std::lock_guard lock(p.inbox_mutex);
                inbox_empty = p.incoming_tasks.empty() && p.incoming_posts.empty();
            }
            if (p.ready.empty() && p.posts.empty() && !inbox_buffered && inbox_empty)
            {
                break;
            }
            continue;
        }
        profiling::scope timer_timing(profiling::stage::timers);
        if (!p.timers.empty() || (p.callbacks && !p.callbacks->callbacks.empty()))
        {
            const auto now = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < dispatch_limit && !p.timers.empty() && p.timers.top()->deadline <= now; ++i)
            {
                p.ready.push_back(p.timers.top()->continuation);
                p.timers.pop();
            }
            for (std::size_t i = 0; i < dispatch_limit && p.callbacks && !p.callbacks->callbacks.empty(); ++i)
            {
                auto it = p.callbacks->callbacks.begin();
                if (it->first.first > now)
                {
                    break;
                }
                auto node = p.callbacks->callbacks.extract(it);
                try
                {
                    node.mapped()();
                }
                catch (...)
                {
                    record_error(std::current_exception());
                }
                if (stopped())
                {
                    break;
                }
            }
        }
        if (stopped())
        {
            continue;
        }
        int timeout = -1;
        if (!p.ready.empty() || !p.posts.empty() || inbox_buffered || inbox_deferred
            || (p.cancellations && p.cancellations->pending.load(std::memory_order_acquire)))
        {
            timeout = 0;
        }
        else if (!p.timers.empty() || (p.callbacks && !p.callbacks->callbacks.empty()))
        {
            auto deadline = p.timers.empty() ? std::chrono::steady_clock::time_point::max() : p.timers.top()->deadline;
            if (p.callbacks && !p.callbacks->callbacks.empty())
            {
                deadline = std::min(deadline, p.callbacks->callbacks.begin()->first.first);
            }
            const auto current = std::chrono::steady_clock::now();
            auto ms =
                deadline <= current ? 0 : std::chrono::ceil<std::chrono::milliseconds>(deadline - current).count();
            timeout = static_cast<int>(std::min<decltype(ms)>(ms, INT_MAX));
        }
        timer_timing.close();
        // Task-only loops have no descriptor readiness to collect. Inbox and
        // timers are still checked on every bounded dispatch turn above.
        if (timeout == 0 && !p.descriptor_count)
        {
            continue;
        }
        try
        {
            if (p.persistent_interests)
            {
                p.reconcile_descriptors();
            }
            const auto count = profiling::measure(
                timeout == 0 ? profiling::stage::poll_nonblocking : profiling::stage::poll_blocking,
                [&] { return p.poller.wait(events, timeout); }
            );
            profiling::scope event_timing(profiling::stage::event_dispatch);
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto& event = events[i];
                auto* found = p.find_descriptor(event.token);
                if (!found)
                {
                    continue;
                }
                auto& state = *found;
                if (event.readable || event.error)
                {
                    p.finish(state, state.reader);
                }
                if (event.writable || event.error)
                {
                    p.finish(state, state.writer);
                }
                if (p.persistent_interests)
                {
                    if (!state.dirty)
                    {
                        p.dirty_descriptors.push_back(event.token);
                        state.dirty = true;
                    }
                }
                else if (state.reader || state.writer)
                {
                    p.update(state, event.token);
                }
            }
        }
        catch (...)
        {
            record_error(std::current_exception());
        }
    }
    if (p.first_error)
    {
        std::rethrow_exception(p.first_error);
    }
}
} // namespace pace
