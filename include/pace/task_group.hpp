#pragma once
#include <pace/operation.hpp>
#include <vector>

namespace pace {
namespace detail {
struct group_state {
    io_context& io;
    cancellation_source source;
    std::size_t pending = 0;
    bool accepting = true;
    bool joining = false;
    std::exception_ptr failure;
    std::vector<std::shared_ptr<void>> resources;
    std::coroutine_handle<> waiter;
    group_state(io_context& owner, cancellation_token parent) : io(owner), source(parent.expires()) { source.link(parent); }
    void check() const { if (io_context::current() != &io) throw std::logic_error("task group is context-affine"); }
    void failed(std::exception_ptr error) {
        if (!failure) failure = error;
        source.request_cancellation();
    }
};
template<class T> struct child_state {
    io_context& io;
    std::optional<T> value;
    std::exception_ptr failure;
    std::coroutine_handle<> waiter;
    bool done = false, consumed = false;
    explicit child_state(io_context& owner) : io(owner) {}
};
template<> struct child_state<void> {
    io_context& io;
    std::exception_ptr failure;
    std::coroutine_handle<> waiter;
    bool done = false, consumed = false;
    explicit child_state(io_context& owner) : io(owner) {}
};
template<class Factory> struct owned_child : child_state<operation_result<Factory>> {
    std::optional<Factory> factory;
    owned_child(io_context& io, Factory work) : child_state<operation_result<Factory>>(io), factory(std::move(work)) {}
};
template<class Factory>
task<void> run_child(std::shared_ptr<group_state> group, std::shared_ptr<owned_child<Factory>> child) {
    try {
        auto work = task_access::bind(invoke_entry(*child->factory, group->io), group->source.token());
        if constexpr (std::is_void_v<operation_result<Factory>>) co_await std::move(work);
        else child->value.emplace(co_await std::move(work));
    } catch (...) { child->failure = std::current_exception(); group->failed(child->failure); }
    child->factory.reset(); // Destroy owned captures before a join can finish.
    child->done = true;
    if (auto waiter = std::exchange(child->waiter, {})) scheduler_access::schedule(group->io, waiter);
    if (--group->pending == 0) {
        if (auto waiter = std::exchange(group->waiter, {})) scheduler_access::schedule(group->io, waiter);
    }
}
struct group_wait {
    std::shared_ptr<group_state> state;
    bool await_ready() const noexcept { return state->pending == 0; }
    void await_suspend(std::coroutine_handle<> h) { state->waiter = h; }
    void await_resume() const noexcept {}
};
inline task<void> drain_group(std::shared_ptr<group_state> state) {
    state->check();
    if (state->joining) throw std::logic_error("task group already joining");
    state->accepting = false;
    state->joining = true;
    co_await group_wait{state};
    state->joining = false;
}
}
// A move-only result handle. Destroying it neither detaches nor cancels the child:
// the enclosing task group still joins it. Await at most once, on the same loop.
template<class T> class [[nodiscard]] join_handle {
    std::shared_ptr<detail::child_state<T>> state_;
    explicit join_handle(std::shared_ptr<detail::child_state<T>> state) : state_(std::move(state)) {}
    friend class task_group;
public:
    join_handle(join_handle&&) noexcept = default;
    join_handle& operator=(join_handle&&) noexcept = default;
    join_handle(const join_handle&) = delete;
    struct awaiter {
        std::shared_ptr<detail::child_state<T>> state;
        bool await_ready() const {
            if (!state) throw std::logic_error("empty join handle");
            if (io_context::current() != &state->io) throw std::logic_error("join handle is context-affine");
            if (state->consumed) throw std::logic_error("join handle already consumed");
            state->consumed = true;
            return state->done;
        }
        void await_suspend(std::coroutine_handle<> h) { state->waiter = h; }
        T await_resume() {
            if (state->failure) std::rethrow_exception(state->failure);
            if constexpr (!std::is_void_v<T>) return std::move(*state->value);
        }
    };
    awaiter operator co_await() & { return {std::move(state_)}; }
    awaiter operator co_await() && { return {std::move(state_)}; }
};
class task_group {
    std::shared_ptr<detail::group_state> state_;
    explicit task_group(std::shared_ptr<detail::group_state> state) : state_(std::move(state)) {}
    friend struct detail::task_access;
public:
    task_group(const task_group&) = delete;
    task_group& operator=(const task_group&) = delete;
    template<class Factory> auto spawn(Factory factory) {
        state_->check();
        if (!state_->accepting) throw std::logic_error("task group is closed");
        state_->source.token().throw_if_cancelled();
        using result = detail::operation_result<Factory>;
        auto child = std::make_shared<detail::owned_child<Factory>>(state_->io, std::move(factory));
        ++state_->pending;
        try { state_->io.spawn(detail::run_child(state_, child)); }
        catch (...) { --state_->pending; throw; }
        return join_handle<result>(std::move(child));
    }
    template<class T> auto spawn(task<T> work) {
        return spawn([work = std::move(work)]() mutable { return std::move(work); });
    }
    // Store scope-local data outside the body coroutine, so exceptional body
    // exit cannot destroy objects still borrowed by children.
    template<class T> T& own(T value) {
        state_->check();
        if (!state_->accepting) throw std::logic_error("task group is closed");
        auto owned = std::make_shared<T>(std::move(value));
        state_->resources.push_back(owned);
        return *owned;
    }
    void cancel() { state_->check(); state_->source.request_cancellation(); }
    cancellation_token token() const noexcept { return state_->source.token(); }
    // Seals the group. Draining is deliberately uncancellable; user code may
    // then inspect results or let the enclosing boundary report the first error.
    task<void> join() { return detail::task_access::bind(detail::drain_group(state_), {}); }
    template<class Factory> static auto run(Factory factory) -> task<typename detail::entry_result<std::invoke_result_t<Factory&, task_group&>>::type> {
        using result = typename detail::entry_result<std::invoke_result_t<Factory&, task_group&>>::type;
        auto token = co_await this_task::cancellation();
        auto state = std::make_shared<detail::group_state>(this_context(), token);
        task_group group(state);
        detail::entry_state<result> value;
        try {
            auto body = detail::task_access::bind(std::invoke(factory, group), state->source.token());
            if constexpr (std::is_void_v<result>) co_await std::move(body);
            else value.value.emplace(co_await std::move(body));
        } catch (...) { state->failed(std::current_exception()); }
        co_await group.join();
        state->resources.clear();
        if (state->failure) std::rethrow_exception(state->failure);
        state->source.token().throw_if_cancelled();
        if constexpr (!std::is_void_v<result>) co_return std::move(*value.value);
    }
};
template<class Factory> auto with_task_group(Factory factory, operation_options options = {}) {
    if (!options.cancellation.stop_possible() && options.deadline.time() == deadline::clock::time_point::max())
        return task_group::run(std::move(factory));
    return with_options([factory = std::move(factory)]() mutable { return task_group::run(std::move(factory)); }, std::move(options));
}
} // namespace pace
