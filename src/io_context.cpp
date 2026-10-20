#include <pace/io_context.hpp>
#include <pace/detail/poller.hpp>
#include <pace/detail/profiling.hpp>
#include <array>
#include <atomic>
#include <climits>
#include <deque>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pace {
namespace profiling = detail::profiling;
namespace {
thread_local io_context* active_context = nullptr;
std::error_code canceled() { return std::make_error_code(std::errc::operation_canceled); }
void check_affinity(io_context* context) {
    if (active_context != context) throw std::logic_error("I/O must execute on its owning io_context");
}
}

struct io_context::implementation {
    detail::poller poller;
    std::atomic<bool> stopping{false};
    std::atomic<bool> running{false};
    bool has_run = false;
    bool cancellation_started = false;
    std::mutex inbox_mutex;
    std::atomic<bool> inbox_pending{false};
    std::deque<task<void>> incoming_tasks;
    std::deque<std::function<void()>> incoming_posts;
    std::deque<std::function<void()>> posts;
    std::deque<std::coroutine_handle<>> ready;
    std::unordered_set<void*> roots;
    std::coroutine_handle<> completed_head{};
    std::exception_ptr first_error;
    struct fd_state {
        io_awaiter* reader = nullptr;
        io_awaiter* writer = nullptr;
        native_socket_type fd = invalid_socket;
    };
    std::unordered_map<std::uint64_t, fd_state> descriptors;
    std::unordered_map<native_socket_type, std::uint64_t> tokens;
    std::uint64_t next_token = 0;
    struct later {
        bool operator()(const timer_awaiter* a, const timer_awaiter* b) const noexcept {
            return a->deadline > b->deadline;
        }
    };
    std::priority_queue<timer_awaiter*, std::vector<timer_awaiter*>, later> timers;

    void finish(io_awaiter*& operation, std::error_code error = {}) {
        if (!operation) return;
        operation->error = error;
        ready.push_back(operation->continuation);
        operation = nullptr;
    }
    void cancel_all() {
        for (auto& [token, state] : descriptors) {
            poller.remove(state.fd);
            finish(state.reader, canceled());
            finish(state.writer, canceled());
        }
        descriptors.clear(); tokens.clear();
        while (!timers.empty()) {
            auto* timer = timers.top(); timers.pop();
            timer->error = canceled(); ready.push_back(timer->continuation);
        }
        cancellation_started = true;
    }
};

io_context::io_context() : impl_(std::make_unique<implementation>()) {}
io_context::~io_context() {
    // Caller must join run() before destruction. Destruct user objects while the
    // entire implementation is alive, and reject reentrant submissions.
    stop();
    impl_->cancel_all();
    impl_->incoming_tasks.clear();
    impl_->incoming_posts.clear();
    impl_->posts.clear();
    for (auto address : impl_->roots) task<void>::handle_type::from_address(address).destroy();
    impl_->roots.clear();
    impl_->ready.clear();
}
io_context* io_context::current() noexcept { return active_context; }
bool io_context::stopped() const noexcept { return impl_->stopping.load(std::memory_order_acquire); }
void io_context::stop() noexcept {
    if (!impl_->stopping.exchange(true, std::memory_order_acq_rel)) impl_->poller.wake();
}
void io_context::spawn(task<void> work) {
    auto& p = *impl_;
    if (current() == this) {
        if (stopped()) throw std::system_error(canceled());
        auto h = work.release();
        if (!h) return;
        try { p.roots.insert(h.address()); p.ready.push_back(h); }
        catch (...) { p.roots.erase(h.address()); h.destroy(); throw; }
        h.promise().on_completion = completed;
        h.promise().completion_context = this;
        return;
    }
    bool notify;
    {
        std::lock_guard lock(p.inbox_mutex);
        if (stopped()) throw std::system_error(canceled());
        p.incoming_tasks.push_back(std::move(work));
        notify = !p.inbox_pending.load(std::memory_order_relaxed);
        p.inbox_pending.store(true, std::memory_order_release);
    }
    if (notify) p.poller.wake();
}
void io_context::post(std::function<void()> work) {
    if (!work) throw std::invalid_argument("empty posted function");
    auto& p = *impl_;
    if (current() == this) {
        if (stopped()) throw std::system_error(canceled());
        p.posts.push_back(std::move(work));
        return;
    }
    bool notify;
    {
        std::lock_guard lock(p.inbox_mutex);
        if (stopped()) throw std::system_error(canceled());
        p.incoming_posts.push_back(std::move(work));
        notify = !p.inbox_pending.load(std::memory_order_relaxed);
        p.inbox_pending.store(true, std::memory_order_release);
    }
    if (notify) p.poller.wake();
}
void io_context::completed(void* context, std::coroutine_handle<> handle) noexcept {
    auto& p = *static_cast<io_context*>(context)->impl_;
    auto h = task<void>::handle_type::from_address(handle.address());
    h.promise().completed_next = p.completed_head;
    p.completed_head = handle;
}
void io_context::schedule(std::coroutine_handle<> handle) {
    check_affinity(this);
    impl_->ready.push_back(handle);
}
bool io_context::io_awaiter::await_ready() noexcept {
    if (context.stopped()) { error = canceled(); return true; }
    return false;
}
bool io_context::io_awaiter::await_suspend(std::coroutine_handle<> handle) { return context.arm(*this, handle); }
void io_context::io_awaiter::await_resume() {
    if (error) throw std::system_error(error);
    if (context.stopped()) throw std::system_error(canceled());
}
bool io_context::arm(io_awaiter& operation, std::coroutine_handle<> handle) {
    profiling::scope timing(profiling::stage::io_arm);
    check_affinity(this);
    if (stopped()) { operation.error = canceled(); return false; }
    auto& p = *impl_;
    auto [token_it, inserted] = p.tokens.try_emplace(operation.fd, ++p.next_token);
    const auto token = token_it->second;
    auto& state = p.descriptors[token];
    state.fd = operation.fd;
    auto& slot = operation.writable ? state.writer : state.reader;
    if (slot) throw std::logic_error("only one pending operation per socket direction is allowed");
    operation.continuation = handle;
    slot = &operation;
    try { p.poller.update(state.fd, state.reader != nullptr, state.writer != nullptr, token); }
    catch (...) {
        slot = nullptr;
        if (inserted) { p.tokens.erase(operation.fd); p.descriptors.erase(token); }
        throw;
    }
    return true;
}
void io_context::cancel(native_socket_type fd) noexcept {
    auto& p = *impl_;
    const auto it = p.tokens.find(fd);
    if (it == p.tokens.end()) return;
    auto state_it = p.descriptors.find(it->second);
    p.poller.remove(fd);
    p.finish(state_it->second.reader, canceled());
    p.finish(state_it->second.writer, canceled());
    p.descriptors.erase(state_it);
    p.tokens.erase(it);
}
bool io_context::timer_awaiter::await_ready() noexcept {
    if (context.stopped()) { error = canceled(); return true; }
    return false; // Even zero-duration sleeps yield, so they cannot starve shutdown.
}
bool io_context::timer_awaiter::await_suspend(std::coroutine_handle<> handle) { return context.arm(*this, handle); }
void io_context::timer_awaiter::await_resume() {
    if (error) throw std::system_error(error);
    if (context.stopped()) throw std::system_error(canceled());
}
bool io_context::arm(timer_awaiter& operation, std::coroutine_handle<> handle) {
    check_affinity(this);
    if (stopped()) { operation.error = canceled(); return false; }
    operation.continuation = handle;
    impl_->timers.push(&operation);
    return true;
}
void io_context::yield_awaiter::await_suspend(std::coroutine_handle<> handle) { context.schedule(handle); }
void io_context::yield_awaiter::await_resume() {
    if (context.stopped()) throw std::system_error(canceled());
}

void io_context::run() {
    auto& p = *impl_;
    if (active_context) throw std::logic_error("nested io_context::run is not supported");
    if (p.running.exchange(true)) throw std::logic_error("io_context already running");
    struct reset_guard {
        implementation& p;
        ~reset_guard() { active_context = nullptr; p.running.store(false); }
    } guard{p};
    if (p.has_run) throw std::logic_error("io_context is single-use");
    p.has_run = true;
    active_context = this;
    profiling::scope run_timing(profiling::stage::reactor_run);
    auto record_error = [&](std::exception_ptr error) {
        try { if (error) std::rethrow_exception(error); }
        catch (const std::system_error& e) {
            if (stopped() && e.code() == std::errc::operation_canceled) return;
            if (!p.first_error) p.first_error = error;
            stop();
        } catch (...) { if (!p.first_error) p.first_error = error; stop(); }
    };
    std::array<detail::poll_event, 256> events{};
    for (;;) {
        profiling::scope inbox_timing(profiling::stage::inbox);
        // Most turns contain no external submissions. Avoid constructing empty
        // deques (which can allocate) and taking the inbox lock on those turns.
        if (p.inbox_pending.load(std::memory_order_acquire)) {
            std::deque<task<void>> new_tasks;
            std::deque<std::function<void()>> new_posts;
            {
                std::lock_guard lock(p.inbox_mutex);
                new_tasks.swap(p.incoming_tasks);
                new_posts.swap(p.incoming_posts);
                // Publish the empty transition under the same lock as enqueue.
                // The first producer after this drain must wake the poller.
                p.inbox_pending.store(false, std::memory_order_relaxed);
            }
            for (auto& work : new_tasks) {
                auto h = work.release();
                if (!h) continue;
                p.roots.insert(h.address());
                h.promise().on_completion = completed;
                h.promise().completion_context = this;
                p.ready.push_back(h);
            }
            for (auto& work : new_posts) p.posts.push_back(std::move(work));
        }
        inbox_timing.close();
        if (stopped() && !p.cancellation_started) p.cancel_all();
        profiling::scope post_timing(profiling::stage::post_dispatch);
        for (std::size_t i = 0, count = std::min<std::size_t>(p.posts.size(), 256); i < count; ++i) {
            auto work = std::move(p.posts.front()); p.posts.pop_front();
            try { work(); } catch (...) { record_error(std::current_exception()); }
        }
        post_timing.close();
        profiling::scope ready_timing(profiling::stage::coroutine_execution);
        for (std::size_t i = 0, count = std::min<std::size_t>(p.ready.size(), 256); i < count; ++i) {
            auto handle = p.ready.front(); p.ready.pop_front();
            handle.resume();
        }
        ready_timing.close();
        profiling::scope reap_timing(profiling::stage::root_reaping);
        while (p.completed_head) {
            auto h = task<void>::handle_type::from_address(p.completed_head.address());
            p.completed_head = h.promise().completed_next;
            record_error(h.promise().exception);
            p.roots.erase(h.address());
            h.destroy();
        }
        reap_timing.close();
        if (stopped()) {
            if (!p.cancellation_started) p.cancel_all();
            bool inbox_empty;
            { std::lock_guard lock(p.inbox_mutex); inbox_empty = p.incoming_tasks.empty() && p.incoming_posts.empty(); }
            if (p.ready.empty() && p.posts.empty() && inbox_empty) break;
            continue;
        }
        profiling::scope timer_timing(profiling::stage::timers);
        const auto now = std::chrono::steady_clock::now();
        while (!p.timers.empty() && p.timers.top()->deadline <= now) {
            p.ready.push_back(p.timers.top()->continuation); p.timers.pop();
        }
        int timeout = -1;
        if (!p.ready.empty() || !p.posts.empty()) timeout = 0;
        else if (!p.timers.empty()) {
            auto ms = std::chrono::ceil<std::chrono::milliseconds>(p.timers.top()->deadline - now).count();
            timeout = static_cast<int>(std::min<decltype(ms)>(ms, INT_MAX));
        }
        timer_timing.close();
        // Task-only loops have no descriptor readiness to collect. Inbox and
        // timers are still checked on every bounded dispatch turn above.
        if (timeout == 0 && p.descriptors.empty()) continue;
        try {
            const auto count = profiling::measure(timeout == 0 ? profiling::stage::poll_nonblocking
                : profiling::stage::poll_blocking, [&] { return p.poller.wait(events, timeout); });
            profiling::scope event_timing(profiling::stage::event_dispatch);
            for (std::size_t i = 0; i < count; ++i) {
                const auto& event = events[i];
                auto it = p.descriptors.find(event.token);
                if (it == p.descriptors.end()) continue;
                auto& state = it->second;
                if (event.readable || event.error) p.finish(state.reader);
                if (event.writable || event.error) p.finish(state.writer);
                if (state.reader || state.writer)
                    p.poller.update(state.fd, state.reader != nullptr, state.writer != nullptr, event.token);
            }
        } catch (...) { record_error(std::current_exception()); }
    }
    if (p.first_error) std::rethrow_exception(p.first_error);
}

} // namespace pace
