#pragma once
#include <pace/runtime/io_context.hpp>

#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace pace
{
/// What a send does when the channel is full.
enum class overflow_policy
{
    reject,      ///< Refuse the value: try_send returns send_status::full, send() and blocking_send() wait for room.
    drop_oldest, ///< Discard the oldest queued value to make room. No send ever waits.
};

/// The outcome of a send.
enum class send_status
{
    sent,           ///< Queued, or handed straight to the waiting receive.
    dropped_oldest, ///< Queued after the oldest queued value was discarded (overflow_policy::drop_oldest).
    full,           ///< Not queued: the channel is full (try_send with overflow_policy::reject).
    closed,         ///< Not queued: the channel is closed.
};

/// Implementation support for channel: its shared state and the waiters of send and receive. Not part of the API.
struct channel_access
{
    /// A suspended send or receive. Its fields are guarded by the channel's mutex while it is linked.
    struct waiter
    {
        waiter* previous = nullptr;
        waiter* next = nullptr;
        bool linked = false;
        io_context* context = nullptr; ///< Where the waiter resumes; null for a thread in blocking_send.
        std::shared_ptr<remote_access::anchor> anchor;
        std::coroutine_handle<> continuation;
        std::error_code error;
        send_status status = send_status::sent;
    };

    /// How to resume a claimed waiter. Filled under the channel lock and used after it is released: once the lock is
    /// gone the waiter's frame may be destroyed by its own context.
    struct wake
    {
        remote_access::anchor* anchor = nullptr;
        std::shared_ptr<remote_access::anchor> keep;
        std::coroutine_handle<> handle;

        void operator()() const
        {
            if (handle)
            {
                remote_access::resume(*anchor, handle);
            }
        }
    };

    template <class T>
    struct state : remote_access::listener
    {
        std::mutex mutex;
        std::condition_variable blocked; ///< Threads in blocking_send.
        std::vector<std::optional<T>> slots;
        std::size_t head = 0;
        std::size_t count = 0;
        overflow_policy overflow;
        bool closed = false;
        io_context* const owner;
        const std::shared_ptr<remote_access::anchor> owner_anchor;
        waiter* receiving = nullptr; ///< The pending receive; then the queue is empty.
        std::optional<T>* receive_slot = nullptr;
        waiter* first = nullptr; ///< Waiting senders, oldest first; then the queue is full.
        waiter* last = nullptr;

        state(io_context& io, std::size_t capacity, overflow_policy policy)
            : slots(capacity)
            , overflow(policy)
            , owner(&io)
            , owner_anchor(remote_access::anchor_of(io))
        {
            notify = [](remote_access::listener& self) noexcept { static_cast<state&>(self).close(true); };
        }

        void push(T&& value) noexcept
        {
            slots[(head + count) % slots.size()].emplace(std::move(value));
            ++count;
        }

        void pop_into(std::optional<T>& out) noexcept
        {
            auto& slot = slots[head];
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): the `count` slots from `head` are engaged
            out.emplace(std::move(*slot));
            slot.reset();
            head = (head + 1) % slots.size();
            --count;
        }

        void append(waiter& entry) noexcept
        {
            entry.previous = last;
            entry.next = nullptr;
            (last ? last->next : first) = &entry;
            last = &entry;
            entry.linked = true;
        }

        void unlink(waiter& entry) noexcept
        {
            (entry.previous ? entry.previous->next : first) = entry.next;
            (entry.next ? entry.next->previous : last) = entry.previous;
            entry.linked = false;
        }

        /// Ends a waiting sender: a coroutine is resumed through `out`, a blocked thread is notified.
        void claim(waiter& entry, wake& out) noexcept
        {
            unlink(entry);
            if (entry.context)
            {
                out.anchor = entry.anchor.get();
                out.keep = std::move(entry.anchor);
                out.handle = entry.continuation;
            }
            else
            {
                blocked.notify_all();
            }
        }

        /// Ends the pending receive, which resumes through `out`.
        void claim_receive(wake& out) noexcept
        {
            out.anchor = owner_anchor.get();
            out.handle = receiving->continuation;
            receiving->linked = false;
            receiving = nullptr;
            receive_slot = nullptr;
        }

        /// Takes the oldest value and refills its slot from the oldest waiting sender, whose wake goes to `out`.
        bool take(std::optional<T>& out, wake& refilled) noexcept
        {
            if (!count)
            {
                return false;
            }
            pop_into(out);
            if (first)
            {
                auto& waiting = static_cast<channel_access::sender<T>&>(*first);
                // NOLINTNEXTLINE(bugprone-unchecked-optional-access): a waiting sender always holds its value
                push(std::move(*waiting.value));
                waiting.status = send_status::sent;
                claim(waiting, refilled);
            }
            return true;
        }

        /// Places `value` if the policy allows it now; moves from `value` only on sent or dropped_oldest. A wake for
        /// the receive goes to `out`, a discarded value to `dropped`. Returns nothing when the sender must wait.
        std::optional<send_status> offer(T& value, wake& out, std::optional<T>& dropped) noexcept
        {
            if (closed)
            {
                return send_status::closed;
            }
            if (receiving)
            {
                receive_slot->emplace(std::move(value));
                claim_receive(out);
                return send_status::sent;
            }
            if (count < slots.size())
            {
                push(std::move(value));
                return send_status::sent;
            }
            if (overflow == overflow_policy::drop_oldest)
            {
                pop_into(dropped);
                push(std::move(value));
                return send_status::dropped_oldest;
            }
            return std::nullopt;
        }

        /// Closes the channel and resumes every waiter. `stopping`: the owner shut down, so a pending receive fails
        /// with operation_canceled instead of reporting the end of the channel. Queued values stay receivable.
        void close(bool stopping) noexcept
        {
            std::scoped_lock lock(mutex);
            if (closed)
            {
                return;
            }
            closed = true;
            // Resume under the lock: a close can wake many waiters, and copying their wakes out would allocate.
            // Delivery takes only the target's anchor and inbox locks, never a channel lock.
            if (receiving)
            {
                if (stopping)
                {
                    receiving->error = std::make_error_code(std::errc::operation_canceled);
                }
                wake receiver;
                claim_receive(receiver);
                receiver();
            }
            while (first)
            {
                first->status = send_status::closed;
                wake sender;
                claim(*first, sender);
                sender();
            }
        }
    };

    /// The awaiter of channel::receive().
    template <class T>
    struct receiver : waiter
    {
        state<T>& channel;
        std::optional<T> value;
        io_context::cancellation_handle registration;
        bool waited = false;

        explicit receiver(state<T>& owner) noexcept
            : channel(owner)
        {
        }

        receiver(const receiver&) = delete;
        receiver& operator=(const receiver&) = delete;

        ~receiver()
        {
            if (waited)
            {
                std::scoped_lock lock(channel.mutex);
                if (linked)
                {
                    channel.receiving = nullptr;
                    channel.receive_slot = nullptr;
                    linked = false;
                }
            }
        }

        // Called with the lock held: takes a value or notes the end of the channel.
        bool settle(wake& refilled) noexcept
        {
            return channel.take(value, refilled) || channel.closed;
        }

        bool await_ready()
        {
            if (remote_access::current() != channel.owner_anchor.get())
            {
                throw std::logic_error("channel receive must run on the owning context");
            }
            if (channel.owner->stopped())
            {
                throw std::system_error(std::make_error_code(std::errc::operation_canceled));
            }
            wake refilled;
            {
                std::scoped_lock lock(channel.mutex);
                if (channel.receiving)
                {
                    throw std::logic_error("channel already has a pending receive");
                }
                if (!settle(refilled))
                {
                    return false;
                }
            }
            refilled();
            return true;
        }

        template <class P>
        bool await_suspend(std::coroutine_handle<P> handle)
        {
            cancellation_token token;
            if constexpr (requires { handle.promise().cancellation; })
            {
                token = handle.promise().cancellation;
            }
            registration = remote_access::on_cancel_or_stop(
                *channel.owner,
                token,
                [this](std::error_code reason) { cancel(reason); }
            );
            wake refilled;
            {
                std::scoped_lock lock(channel.mutex);
                if (!settle(refilled))
                {
                    continuation = handle;
                    channel.receiving = this;
                    channel.receive_slot = &value;
                    linked = true;
                    waited = true;
                    return true;
                }
            }
            registration.cancel();
            refilled();
            return false;
        }

        // Runs on the owner, for the token or the owner's stop; a receive already claimed by a sender ignores it.
        void cancel(std::error_code reason)
        {
            {
                std::scoped_lock lock(channel.mutex);
                if (!linked)
                {
                    return;
                }
                channel.receiving = nullptr;
                channel.receive_slot = nullptr;
                linked = false;
                error = reason;
            }
            scheduler_access::schedule(*channel.owner, continuation);
        }

        std::optional<T> await_resume()
        {
            registration.cancel();
            if (error)
            {
                throw std::system_error(error);
            }
            return std::move(value);
        }
    };

    /// The awaiter of channel::send(), and the waiting thread of blocking_send().
    template <class T>
    struct sender : waiter
    {
        state<T>& channel;
        std::optional<T> value;
        io_context::cancellation_handle registration;
        bool waited = false;

        sender(state<T>& owner, T item) noexcept
            : channel(owner)
            , value(std::move(item))
        {
        }

        sender(const sender&) = delete;
        sender& operator=(const sender&) = delete;

        ~sender()
        {
            if (waited)
            {
                std::scoped_lock lock(channel.mutex);
                if (linked)
                {
                    channel.unlink(*this);
                }
            }
        }

        bool await_ready()
        {
            wake receiver;
            std::optional<T> dropped;
            std::optional<send_status> result;
            {
                std::scoped_lock lock(channel.mutex);
                // NOLINTNEXTLINE(bugprone-unchecked-optional-access): engaged until a send takes it
                result = channel.offer(*value, receiver, dropped);
            }
            if (!result)
            {
                return false;
            }
            status = *result;
            receiver();
            return true;
        }

        template <class P>
        bool await_suspend(std::coroutine_handle<P> handle)
        {
            auto* here = io_context::current();
            if (!here)
            {
                throw std::logic_error("channel send must run on a PACE context");
            }
            cancellation_token token;
            if constexpr (requires { handle.promise().cancellation; })
            {
                token = handle.promise().cancellation;
            }
            registration =
                remote_access::on_cancel_or_stop(*here, token, [this](std::error_code reason) { cancel(reason); });
            context = here;
            anchor = remote_access::anchor_of(*here);
            continuation = handle;
            wake receiver;
            std::optional<T> dropped;
            std::optional<send_status> result;
            {
                std::scoped_lock lock(channel.mutex);
                // NOLINTNEXTLINE(bugprone-unchecked-optional-access): engaged until a send takes it
                result = channel.offer(*value, receiver, dropped);
                if (!result)
                {
                    channel.append(*this);
                    waited = true;
                    return true;
                }
            }
            status = *result;
            registration.cancel();
            receiver();
            return false;
        }

        // Runs on the sender's context, for its token or its context's stop; a claimed send ignores it.
        void cancel(std::error_code reason)
        {
            {
                std::scoped_lock lock(channel.mutex);
                if (!linked)
                {
                    return;
                }
                channel.unlink(*this);
                error = reason;
            }
            scheduler_access::schedule(*context, continuation);
        }

        send_status await_resume()
        {
            registration.cancel();
            if (error)
            {
                throw std::system_error(error);
            }
            return status;
        }
    };
};

/// A bounded multi-producer, single-consumer queue of T owned by one io_context. Any thread may send; only a
/// coroutine on the owner receives. A channel is a handle: copies share one queue, which lives until the last copy
/// is destroyed. The owner's shutdown (the start of its stop drain, or its destruction) closes the channel.
/// T must be nothrow move constructible and nothrow destructible.
template <class T>
class channel
{
    static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_destructible_v<T>);
    using state = channel_access::state<T>;
    std::shared_ptr<state> state_;

public:
    using value_type = T;

    /// A channel of `capacity` values received on `owner`; all slots are allocated now. Thread-safe; `owner` need
    /// not be running yet. A channel created after the owner shut down starts closed. Throws std::invalid_argument
    /// for a capacity of 0.
    channel(io_context& owner, std::size_t capacity, overflow_policy overflow = overflow_policy::reject)
        : state_(make(owner, capacity, overflow))
    {
    }

    /// Shares the queue.
    channel(const channel&) = default;
    /// Shares `other`'s queue.
    channel& operator=(const channel&) = default;
    /// Takes the handle; a moved-from channel may only be assigned or destroyed.
    channel(channel&&) noexcept = default;
    /// Takes the handle; a moved-from channel may only be assigned or destroyed.
    channel& operator=(channel&&) noexcept = default;
    /// Releases this handle; the queue and any values in it go with the last one.
    ~channel() = default;

    /// Sends without waiting, from any thread. Moves from `value` only on sent or dropped_oldest. Resumes a
    /// pending receive on the owner. May throw std::bad_alloc while waking it, after the value was queued.
    // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved): offer() moves only when it queues
    send_status try_send(T&& value)
    {
        channel_access::wake receiver;
        std::optional<T> dropped;
        std::optional<send_status> result;
        {
            std::scoped_lock lock(state_->mutex);
            result = state_->offer(value, receiver, dropped);
        }
        receiver();
        return result ? *result : send_status::full;
    }

    /// try_send of a copy of `value`.
    send_status try_send(const T& value)
    {
        T copy(value);
        return try_send(std::move(copy));
    }

    /// Awaitable send for a coroutine on any running context. When the channel is full under overflow_policy::reject
    /// it waits, in order with other waiting senders, until the receiver makes room, and resumes on the sender's own
    /// context. Returns sent, dropped_oldest or closed (the value is then destroyed). Throws the task token's error
    /// on cancellation and operation_canceled when the sender's context stops while waiting, with the value not
    /// sent; std::logic_error on a thread that runs no context. This handle must outlive the co_await.
    channel_access::sender<T> send(T value) noexcept
    {
        return {*state_, std::move(value)};
    }

    /// Sends from a thread that runs no PACE context, waiting for room when the channel is full under
    /// overflow_policy::reject. Returns sent, dropped_oldest or closed. Throws std::logic_error on a thread running a
    /// context (a context must never block; use send()), and the token's error when `cancellation` is cancelled or
    /// its deadline passes while waiting. Waits until then if the owner never receives, closes or shuts down.
    send_status blocking_send(T value, const cancellation_token& cancellation = {})
    {
        if (io_context::current())
        {
            throw std::logic_error("blocking_send called on a PACE context; use co_await send()");
        }
        cancellation.throw_if_cancelled();
        auto& s = *state_;
        channel_access::sender<T> node(s, std::move(value));
        cancellation_registration registration(
            cancellation,
            [&]
            {
                std::scoped_lock lock(s.mutex);
                if (node.linked)
                {
                    s.unlink(node);
                    node.error = cancellation.error();
                    s.blocked.notify_all();
                }
            }
        );
        channel_access::wake receiver;
        std::optional<T> dropped;
        std::unique_lock lock(s.mutex);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): engaged until a send takes it
        if (auto result = s.offer(*node.value, receiver, dropped))
        {
            lock.unlock();
            receiver();
            return *result;
        }
        if (auto error = cancellation.error())
        {
            throw std::system_error(error);
        }
        s.append(node);
        node.waited = true;
        const auto until = cancellation.expires().time();
        while (node.linked)
        {
            if (until == deadline::clock::time_point::max())
            {
                s.blocked.wait(lock);
            }
            else if (s.blocked.wait_until(lock, until) == std::cv_status::timeout && node.linked)
            {
                s.unlink(node);
                node.error = std::make_error_code(std::errc::timed_out);
            }
        }
        lock.unlock();
        if (node.error)
        {
            throw std::system_error(node.error);
        }
        return node.status;
    }

    /// Awaitable receive, on the owner only: the oldest value, or std::nullopt once the channel is closed and empty.
    /// One receive at a time. Throws std::logic_error on another thread or for a second pending receive, the task
    /// token's error on cancellation, and operation_canceled once the owner is stopped. This handle must outlive
    /// the co_await.
    channel_access::receiver<T> receive() noexcept
    {
        return channel_access::receiver<T>(*state_);
    }

    /// The oldest value without waiting, or std::nullopt when the channel is empty. Owner only (std::logic_error
    /// otherwise); usable while the owner drains after stop().
    std::optional<T> try_receive()
    {
        if (remote_access::current() != state_->owner_anchor.get())
        {
            throw std::logic_error("channel receive must run on the owning context");
        }
        std::optional<T> value;
        channel_access::wake refilled;
        {
            std::scoped_lock lock(state_->mutex);
            state_->take(value, refilled);
        }
        refilled();
        return value;
    }

    /// Closes the channel from any thread: later sends return closed, waiting senders resume with closed, and a
    /// pending receive resumes with std::nullopt once no values are queued. Idempotent.
    void close() noexcept
    {
        state_->close(false);
    }

    /// Whether the channel is closed. Thread-safe.
    bool closed() const
    {
        std::scoped_lock lock(state_->mutex);
        return state_->closed;
    }

    /// The number of queued values. Thread-safe; may change at once.
    std::size_t size() const
    {
        std::scoped_lock lock(state_->mutex);
        return state_->count;
    }

    /// The capacity given at construction.
    std::size_t capacity() const noexcept
    {
        return state_->slots.size();
    }

private:
    static std::shared_ptr<state> make(io_context& owner, std::size_t capacity, overflow_policy overflow)
    {
        if (capacity == 0)
        {
            throw std::invalid_argument("channel capacity must be positive");
        }
        auto created = std::make_shared<state>(owner, capacity, overflow);
        if (!remote_access::listen(*created->owner_anchor, created))
        {
            created->closed = true;
        }
        return created;
    }
};
} // namespace pace
