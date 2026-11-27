#pragma once
#include <pace/runtime/operation.hpp>

#include <list>
#include <vector>

namespace pace
{
/// Implementation support for task_group and join_handle: the group's and children's shared state and the wait for a
/// group to drain. Not part of the API.
struct group_access
{
    struct wait;

    /// The shared state of a task group.
    struct state
    {
        io_context& io;
        cancellation_source source;
        std::size_t pending = 0;
        bool accepting = true;
        bool joining = false;
        bool cancelled_locally = false;
        std::exception_ptr failure;
        std::vector<std::shared_ptr<void>> resources;
        std::list<wait*> waiters;

        state(io_context& owner, cancellation_token parent)
            : io(owner)
            , source(parent.expires())
        {
            source.link(std::move(parent));
        }

        void check() const
        {
            if (io_context::current() != &io)
            {
                throw std::logic_error("task group is context-affine");
            }
        }

        /// Records the first failure and cancels the group. operation_canceled caused by the group's own cancel()
        /// is the expected outcome of that request, not a failure.
        void failed(std::exception_ptr error)
        {
            if (!failure && !(cancelled_locally && is_cancellation(error)))
            {
                failure = std::move(error);
            }
            source.request_cancellation();
        }

        void child_finished()
        {
            if (--pending != 0)
            {
                return;
            }
            while (!waiters.empty())
            {
                auto* waiting = waiters.front();
                waiters.pop_front();
                waiting->registered = false;
                scheduler_access::schedule(io, waiting->continuation);
            }
        }

        static bool is_cancellation(const std::exception_ptr& error)
        {
            try
            {
                std::rethrow_exception(error);
            }
            catch (const std::system_error& failure)
            {
                return failure.code() == std::errc::operation_canceled;
            }
            catch (...)
            {
                return false;
            }
        }
    };

    /// The shared state of one child and its join_handle.
    template <class T>
    struct child
    {
        io_context& io;
        std::optional<T> value;
        std::exception_ptr failure;
        std::coroutine_handle<> waiter;
        bool done = false, consumed = false;

        explicit child(io_context& owner)
            : io(owner)
        {
        }
    };

    /// A child's state together with its owned factory.
    template <class Factory>
    struct owned_child : child<operation_result<Factory>>
    {
        std::optional<Factory> factory;

        owned_child(io_context& io, Factory work)
            : child<operation_result<Factory>>(io)
            , factory(std::move(work))
        {
        }
    };

    /// Waits until a group has no running children.
    struct wait
    {
        state& group; // drain's frame owns the state throughout this wait.
        bool cancellable = false;
        bool registered = false;
        std::list<wait*>::iterator position;
        std::coroutine_handle<> continuation;
        io_context::cancellation_handle cancellation;
        std::error_code error;

        wait(state& owner, bool can_cancel)
            : group(owner)
            , cancellable(can_cancel)
        {
        }

        wait(const wait&) = delete;
        wait& operator=(const wait&) = delete;

        ~wait()
        {
            if (registered)
            {
                group.waiters.erase(position);
            }
        }

        bool await_ready() const noexcept
        {
            return group.pending == 0;
        }

        void await_suspend(std::coroutine_handle<> h)
        {
            if (cancellable)
            {
                cancellation = group.io.on_cancel(
                    group.source.token(),
                    [this](std::error_code reason)
                    {
                        error = reason;
                        if (registered)
                        {
                            group.waiters.erase(position);
                            registered = false;
                            scheduler_access::schedule(group.io, continuation);
                        }
                    }
                );
            }
            continuation = h;
            position = group.waiters.insert(group.waiters.end(), this);
            registered = true;
        }

        void await_resume()
        {
            cancellation.cancel();
            if (error)
            {
                throw std::system_error(error);
            }
        }
    };
};

template <>
struct group_access::child<void>
{
    io_context& io;
    std::exception_ptr failure;
    std::coroutine_handle<> waiter;
    bool done = false, consumed = false;

    explicit child(io_context& owner)
        : io(owner)
    {
    }
};

// Coroutines in public headers stay namespace-scope functions: GCC 16 cannot read a class-scope coroutine back
// from a named-module interface ("failed to read compiled module cluster"), so these are not members of the
// *_access struct above.

/// Implementation support: the root task that runs one child and records its outcome. Not part of the API.
template <class Factory>
task<void> run_group_child(
    std::shared_ptr<group_access::state> group,
    std::shared_ptr<group_access::owned_child<Factory>> owned
)
{
    try
    {
        // spawn() constructs the child with its factory; only this coroutine resets it, below.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): engaged until the reset after this block
        auto work = task_access::bind(entry_access::invoke(*owned->factory, group->io), group->source.token());
        if constexpr (std::is_void_v<operation_result<Factory>>)
        {
            co_await std::move(work);
        }
        else
        {
            owned->value.emplace(co_await std::move(work));
        }
    }
    catch (...)
    {
        owned->failure = std::current_exception();
        group->failed(owned->failure);
    }
    owned->factory.reset(); // Destroy owned captures before a join can finish.
    owned->done = true;
    if (auto waiter = std::exchange(owned->waiter, {}))
    {
        scheduler_access::schedule(group->io, waiter);
    }
    group->child_finished();
}

/// Implementation support: waits for a closed group's children (uncancellable at the scope boundary). Not part of
/// the API.
inline task<void> drain_group(std::shared_ptr<group_access::state> group, bool boundary = false)
{
    struct reset
    {
        group_access::state& group;
        bool boundary;

        reset(group_access::state& owner, bool at_boundary) noexcept
            : group(owner)
            , boundary(at_boundary)
        {
        }

        reset(const reset&) = delete;
        reset& operator=(const reset&) = delete;

        ~reset()
        {
            if (!boundary)
            {
                group.joining = false;
            }
        }
    } guard{*group, boundary};

    if (!boundary)
    {
        group->joining = true;
    }
    co_await group_access::wait(*group, !boundary);
}

/// The move-only result of one child. co_await returns its value or rethrows its exception. Await at most once, on
/// the group's context (std::logic_error otherwise). Destroying it neither detaches nor cancels the child.
template <class T>
class [[nodiscard]] join_handle
{
    std::shared_ptr<group_access::child<T>> state_;

    explicit join_handle(std::shared_ptr<group_access::child<T>> state)
        : state_(std::move(state))
    {
    }
    friend class task_group;

public:
    join_handle(join_handle&&) noexcept = default;
    join_handle& operator=(join_handle&&) noexcept = default;
    join_handle(const join_handle&) = delete;
    join_handle& operator=(const join_handle&) = delete;
    ~join_handle() = default;

    /// Awaits the child's completion; opaque.
    struct awaiter
    {
        std::shared_ptr<group_access::child<T>> state;

        bool await_ready() const
        {
            if (!state)
            {
                throw std::logic_error("empty join handle");
            }
            if (io_context::current() != &state->io)
            {
                throw std::logic_error("join handle is context-affine");
            }
            if (state->consumed)
            {
                throw std::logic_error("join handle already consumed");
            }
            state->consumed = true;
            return state->done;
        }

        void await_suspend(std::coroutine_handle<> h)
        {
            state->waiter = h;
        }

        T await_resume()
        {
            if (state->failure)
            {
                std::rethrow_exception(state->failure);
            }
            if constexpr (!std::is_void_v<T>)
            {
                return std::move(*state->value);
            }
        }
    };

    awaiter operator co_await() &
    {
        return {std::move(state_)};
    }

    awaiter operator co_await() &&
    {
        return {std::move(state_)};
    }
};

/// A scope of children on one context, created only by task_group::run and with_task_group. Not copyable or movable.
/// Every member must be used on the group's context (std::logic_error otherwise).
class task_group
{
    std::shared_ptr<group_access::state> state_;

    explicit task_group(std::shared_ptr<group_access::state> state)
        : state_(std::move(state))
    {
    }
    friend struct task_access;

public:
    task_group(const task_group&) = delete;
    task_group& operator=(const task_group&) = delete;
    ~task_group() = default;

    /// Owns `factory` (taking io_context& or nothing, returning task<T>), starts the child on a later turn with the
    /// group's token and returns join_handle<T>. Throws std::logic_error after join started, and std::system_error
    /// if the group is cancelled or the context stopped.
    template <class Factory>
    auto spawn(Factory factory)
    {
        state_->check();
        if (!state_->accepting)
        {
            throw std::logic_error("task group is closed");
        }
        state_->source.token().throw_if_cancelled();
        using result = operation_result<Factory>;
        auto child = std::make_shared<group_access::owned_child<Factory>>(state_->io, std::move(factory));
        ++state_->pending;
        try
        {
            state_->io.spawn(run_group_child(state_, child));
        }
        catch (...)
        {
            --state_->pending;
            throw;
        }
        return join_handle<result>(std::move(child));
    }

    /// spawn for an already-created task; what the task borrowed stays borrowed.
    template <class T>
    auto spawn(task<T> work)
    {
        return spawn([work = std::move(work)]() mutable { return std::move(work); });
    }

    /// Moves `value` into group storage, kept until every child has finished, and returns a reference to it. Use it
    /// for data children borrow. Throws std::logic_error after join started.
    template <class T>
    T& own(T value)
    {
        state_->check();
        if (!state_->accepting)
        {
            throw std::logic_error("task group is closed");
        }
        auto owned = std::make_shared<T>(std::move(value));
        auto& result = *owned;
        state_->resources.push_back(std::move(owned));
        return result;
    }

    /// Cancels the group's token: the body and children see operation_canceled. The scope still joins them. Their
    /// operation_canceled is the expected result of this request, so the scope then returns normally: the body's
    /// value if it produced one (a value-returning body that did not throws operation_canceled), unless the body or
    /// a child failed with another exception, or the enclosing operation was cancelled too.
    void cancel()
    {
        state_->check();
        state_->cancelled_locally = true;
        state_->source.request_cancellation();
    }

    /// The group's token.
    cancellation_token token() const noexcept
    {
        return state_->source.token();
    }

    /// Closes the group at once and waits for every child; does not rethrow their exceptions. Throws the group
    /// token's error if it is cancelled, std::logic_error for a second concurrent join. Never join from one of the
    /// group's own children. The scope always drains the children again, uncancellably.
    task<void> join()
    {
        state_->check();
        if (state_->joining)
        {
            throw std::logic_error("task group already joining");
        }
        // Close the group here, not in the task: awaiting a task bound to an already-cancelled token throws before
        // its body runs, and the group must be closed either way.
        state_->accepting = false;
        return task_access::bind(drain_group(state_), state_->source.token());
    }

    /// The scope: runs factory(group) as the body, joins every child, destroys own() storage, then rethrows the
    /// first exception of the body or a child, throws if the group was cancelled, or returns the body's value.
    template <class Factory>
    static auto run(Factory factory) -> task<entry_access::result<std::invoke_result_t<Factory&, task_group&>>>
    {
        using result = entry_access::result<std::invoke_result_t<Factory&, task_group&>>;
        auto parent = co_await this_task::cancellation();
        task_group group(std::make_shared<group_access::state>(this_context(), parent));
        const auto& state = group.state_;
        entry_access::state<result> value;
        try
        {
            auto body = task_access::bind(std::invoke(factory, group), state->source.token());
            // settle: a value the body returned after the group's own cancel() is still the scope's result.
            if constexpr (std::is_void_v<result>)
            {
                co_await task_access::settle(std::move(body));
            }
            else
            {
                value.value.emplace(co_await task_access::settle(std::move(body)));
            }
        }
        catch (...)
        {
            state->failed(std::current_exception());
        }
        if (state->joining)
        {
            state->failed(
                std::make_exception_ptr(std::logic_error("task group body exited with another join pending"))
            );
        }
        state->accepting = false;
        co_await task_access::bind(drain_group(state, true), {});
        state->resources.clear();
        if (state->failure)
        {
            std::rethrow_exception(state->failure);
        }
        // Cancellation of the enclosing operation propagates. The group's own cancel() does not: it ends the scope
        // early with whatever the body produced.
        parent.throw_if_cancelled();
        if (!state->cancelled_locally)
        {
            state->source.token().throw_if_cancelled();
        }
        if constexpr (!std::is_void_v<result>)
        {
            if (!value.value)
            {
                throw std::system_error(std::make_error_code(std::errc::operation_canceled));
            }
            co_return std::move(*value.value);
        }
    }
};

/// task_group::run(factory), inside with_options(…, options) when `options` is not empty.
template <class Factory>
auto with_task_group(Factory factory, operation_options options = {})
{
    if (!options.cancellation.stop_possible() && options.deadline.time() == deadline::clock::time_point::max())
    {
        return task_group::run(std::move(factory));
    }
    return with_options(
        [factory = std::move(factory)]() mutable { return task_group::run(std::move(factory)); },
        std::move(options)
    );
}
} // namespace pace
