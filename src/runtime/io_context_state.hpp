#pragma once

// Internal: io_context's private state, shared by the io_context*.cpp files.
#include <pace/runtime/io_context.hpp>
#include "runtime/poller.hpp"
#include "runtime/profiling.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace pace
{
// The context running on this thread. constinit lets other translation units read it without a
// thread_local initialization wrapper on every access.
extern constinit thread_local io_context* active_context;

inline constexpr std::size_t dispatch_limit = 256;

inline std::error_code canceled()
{
    return std::make_error_code(std::errc::operation_canceled);
}

inline void check_affinity(io_context* context)
{
    if (active_context != context)
    {
        throw std::logic_error("I/O must execute on its owning io_context");
    }
}

struct io_context::callback_timers
{
    using key = std::pair<std::chrono::steady_clock::time_point, std::uint64_t>;
    std::map<key, std::function<void()>> callbacks;
    std::uint64_t next_id = 0;

    void clear() noexcept
    {
        // Extract before destroying captures: a capture may own another handle.
        while (!callbacks.empty())
        {
            auto node = callbacks.extract(callbacks.begin());
        }
    }
};

struct io_context::cancellation_records
{
    struct record
    {
        cancellation_token token;
        std::function<void(std::error_code)> callback;
        std::uint64_t id = 0;
        record* previous = nullptr;
        record* next = nullptr;
        bool queued = false;
        std::optional<pace::cancellation_registration> wake;
    };

    pace::poller& poller;
    std::mutex pending_mutex;
    record* first = nullptr;
    record* last = nullptr;
    std::atomic<bool> pending{false};
    std::map<std::uint64_t, record> entries;
    std::uint64_t next_id = 0;

    explicit cancellation_records(pace::poller& wake_poller)
        : poller(wake_poller)
    {
    }

    ~cancellation_records()
    {
        while (!entries.empty())
        {
            auto node = entries.extract(entries.begin());
            detach(node.mapped());
        }
    }

    // Notifications can run on any thread. The record already owns the queue
    // links, so requesting cancellation cannot allocate.
    void enqueue(record& entry) noexcept
    {
        bool notify;
        {
            std::lock_guard lock(pending_mutex);
            notify = !first;
            entry.queued = true;
            entry.previous = last;
            if (last)
            {
                last->next = &entry;
            }
            else
            {
                first = &entry;
            }
            last = &entry;
            if (notify)
            {
                pending.store(true, std::memory_order_release);
            }
        }
        if (notify)
        {
            poller.wake();
        }
    }

    void unlink(record& entry) noexcept
    {
        if (entry.previous)
        {
            entry.previous->next = entry.next;
        }
        else
        {
            first = entry.next;
        }
        if (entry.next)
        {
            entry.next->previous = entry.previous;
        }
        else
        {
            last = entry.previous;
        }
        entry.queued = false;
        if (!first)
        {
            pending.store(false, std::memory_order_relaxed);
        }
    }

    std::uint64_t take_pending() noexcept
    {
        if (!pending.load(std::memory_order_acquire))
        {
            return 0;
        }
        std::lock_guard lock(pending_mutex);
        if (!first)
        {
            return 0;
        }
        const auto id = first->id;
        unlink(*first);
        return id;
    }

    void detach(record& entry) noexcept
    {
        // Wait for any notifier before releasing the record. Once reset
        // returns, only the reactor can change this record's queued flag.
        entry.wake.reset();
        if (entry.queued)
        {
            std::lock_guard lock(pending_mutex);
            unlink(entry);
        }
    }

    void cancel(std::uint64_t id) noexcept
    {
        auto node = entries.extract(id);
        if (!node.empty())
        {
            detach(node.mapped());
        }
    }

    std::exception_ptr dispatch(bool shutdown = false)
    {
        std::exception_ptr failure;
        for (std::size_t i = 0; shutdown || i < dispatch_limit; ++i)
        {
            auto id = take_pending();
            if (!id && shutdown && !entries.empty())
            {
                id = entries.begin()->first;
            }
            if (!id)
            {
                break;
            }
            // Extract before invoking a callback that may unregister other records.
            auto node = entries.extract(id);
            auto& entry = node.mapped();
            detach(entry);
            auto error = entry.token.error();
            // Shutdown supplies a reason only if the source has none of its own.
            if (shutdown && !error)
            {
                error = canceled();
            }
            try
            {
                entry.callback(error);
            }
            catch (...)
            {
                if (!failure)
                {
                    failure = std::current_exception();
                }
            }
        }
        return failure;
    }
};

struct io_context::implementation
{
    pace::poller poller;
    const bool persistent_interests;

    explicit implementation(io_backend backend)
        : poller(pace::poller::supports_persistent, backend)
        , persistent_interests(poller.persistent())
    {
    }

    std::atomic<bool> stopping{false};
    std::atomic<bool> running{false};
    bool has_run = false;
    bool cancellation_started = false;
    std::shared_ptr<cancellation_records> cancellations;
    std::mutex inbox_mutex;
    std::atomic<bool> inbox_pending{false};
    std::deque<task<void>> incoming_tasks;
    std::deque<std::function<void()>> incoming_posts;

    // Called with inbox_mutex held, after enqueue.
    bool mark_inbox() noexcept
    {
        const bool notify = !inbox_pending.load(std::memory_order_relaxed);
        if (notify)
        {
            inbox_pending.store(true, std::memory_order_release);
        }
        return notify;
    }

    template <class Work, class Promote>
    static void promote_batch(std::deque<Work>& batch, Promote promote)
    {
        if (batch.size() <= dispatch_limit)
        {
            for (auto& work : batch)
            {
                promote(work);
            }
            batch.clear();
            return;
        }
        for (std::size_t i = 0; i < dispatch_limit; ++i)
        {
            promote(batch.front());
            batch.pop_front();
        }
    }

    std::deque<std::function<void()>> posts;
    std::deque<std::coroutine_handle<>> ready;
    std::vector<task<void>::handle_type> roots;

    void adopt_root(io_context& io, task<void>& work)
    {
        const auto handle = work.release();
        if (!handle)
        {
            return;
        }
        auto& promise = handle.promise();
        promise.root_index = roots.size();
        try
        {
            roots.push_back(handle);
            ready.push_back(handle);
        }
        catch (...)
        {
            if (roots.size() > promise.root_index)
            {
                roots.pop_back();
            }
            handle.destroy();
            throw;
        }
        promise.on_completion = io_context::completed;
        promise.completion_context = &io;
    }

    std::coroutine_handle<> completed_head{};
    std::exception_ptr first_error;

    struct service
    {
        std::shared_ptr<const void> key;
        std::shared_ptr<void> value;
    };

    std::unordered_map<const void*, service> services;

    struct fd_state
    {
        io_awaiter* reader = nullptr;
        io_awaiter* writer = nullptr;
        native_socket_type fd = invalid_socket;
        unsigned interests = 0;
        bool dirty = false;
        std::uint64_t token = 0;
    };

    // Dense reusable slots: event dispatch is a checked array lookup instead
    // of a second node-based hash lookup. Generations reject stale OS events.
    std::vector<fd_state> descriptors;
    std::vector<std::uint32_t> free_descriptors;
    std::size_t descriptor_count = 0;
    std::unordered_map<native_socket_type, std::uint64_t> tokens;
    std::vector<std::uint64_t> dirty_descriptors;

    // Cancellation can remove any sleeper, including thousands at once. Keep
    // its slot in the coroutine frame to avoid a scan and heap rebuild.
    struct timer_queue
    {
        std::vector<timer_awaiter*> heap;

        bool empty() const noexcept
        {
            return heap.empty();
        }

        timer_awaiter* top() const noexcept
        {
            return heap.front();
        }

        void place(std::size_t slot, timer_awaiter* timer) noexcept
        {
            heap[slot] = timer;
            timer->heap_index = slot;
        }

        void sift_up(std::size_t slot, timer_awaiter* timer) noexcept
        {
            while (slot)
            {
                const auto parent = (slot - 1) / 2;
                if (heap[parent]->deadline <= timer->deadline)
                {
                    break;
                }
                place(slot, heap[parent]);
                slot = parent;
            }
            place(slot, timer);
        }

        void push(timer_awaiter* timer)
        {
            heap.push_back(timer);
            sift_up(heap.size() - 1, timer);
        }

        bool erase(timer_awaiter* timer) noexcept
        {
            auto slot = timer->heap_index;
            if (slot >= heap.size() || heap[slot] != timer)
            {
                return false;
            }
            auto* last = heap.back();
            heap.pop_back();
            timer->heap_index = static_cast<std::size_t>(-1);
            if (slot == heap.size())
            {
                return true;
            }
            if (slot && last->deadline < heap[(slot - 1) / 2]->deadline)
            {
                sift_up(slot, last);
                return true;
            }
            while (slot < heap.size() / 2)
            {
                auto child = 2 * slot + 1;
                if (child + 1 < heap.size() && heap[child + 1]->deadline < heap[child]->deadline)
                {
                    ++child;
                }
                if (last->deadline <= heap[child]->deadline)
                {
                    break;
                }
                place(slot, heap[child]);
                slot = child;
            }
            place(slot, last);
            return true;
        }

        void pop() noexcept
        {
            erase(top());
        }
    } timers;

    std::shared_ptr<callback_timers> callbacks;

    fd_state* find_descriptor(std::uint64_t token) noexcept
    {
        const auto index = static_cast<std::uint32_t>(token);
        if (index >= descriptors.size())
        {
            return nullptr;
        }
        auto& state = descriptors[index];
        return state.fd != invalid_socket && state.token == token ? &state : nullptr;
    }

    std::uint64_t add_descriptor(native_socket_type fd)
    {
        std::uint32_t index;
        if (!free_descriptors.empty())
        {
            index = free_descriptors.back();
            free_descriptors.pop_back();
        }
        else
        {
            if (descriptors.size() == UINT32_MAX)
            {
                throw std::overflow_error("descriptor slots exhausted");
            }
            if (descriptors.size() == descriptors.capacity())
            {
                const auto capacity = std::max<std::size_t>(16, descriptors.size() * 2);
                free_descriptors.reserve(capacity);
                descriptors.reserve(capacity);
            }
            index = static_cast<std::uint32_t>(descriptors.size());
            descriptors.emplace_back();
        }
        auto& state = descriptors[index];
        const auto generation = (state.token >> 32) + 1;
        state.fd = fd;
        state.token = (generation << 32) | index;
        ++descriptor_count;
        return state.token;
    }

    void erase_descriptor(std::uint64_t token) noexcept
    {
        if (auto* state = find_descriptor(token))
        {
            // Retire an exhausted slot so its previous tokens cannot recur.
            *state = {.token = token};
            if ((token >> 32) != UINT32_MAX)
            {
                free_descriptors.push_back(static_cast<std::uint32_t>(token));
            }
            --descriptor_count;
        }
    }

    void finish(fd_state& state, io_awaiter* operation, std::error_code error = {})
    {
        if (!operation)
        {
            return;
        }
        operation->error = error;
        ready.push_back(operation->continuation);
        // wait_ready owns both slots. Clear both before another direction from
        // the same event (or cancel_all) can enqueue the continuation again.
        if (state.reader == operation)
        {
            state.reader = nullptr;
        }
        if (state.writer == operation)
        {
            state.writer = nullptr;
        }
    }

    void cancel_all(bool running_on_owner)
    {
        for (auto& state : descriptors)
        {
            if (state.fd == invalid_socket)
            {
                continue;
            }
            poller.remove(state.fd);
            finish(state, state.reader, canceled());
            finish(state, state.writer, canceled());
        }
        descriptors.clear();
        free_descriptors.clear();
        descriptor_count = 0;
        tokens.clear();
        dirty_descriptors.clear();
        while (!timers.empty())
        {
            auto* timer = timers.top();
            timers.pop();
            timer->error = canceled();
            ready.push_back(timer->continuation);
        }
        if (callbacks)
        {
            callbacks->clear();
        }
        cancellation_started = true;
        if (cancellations && running_on_owner)
        {
            auto failure = cancellations->dispatch(true);
            if (!first_error)
            {
                first_error = failure;
            }
        }
    }

    void update(fd_state& state, std::uint64_t token)
    {
        const unsigned interests = (state.reader ? 1U : 0U) | (state.writer ? 2U : 0U);
        if (persistent_interests)
        {
            if (interests == state.interests)
            {
                return;
            }
        }
        poller.update(state.fd, state.reader != nullptr, state.writer != nullptr, token);
        state.interests = interests;
    }

    void reconcile_descriptors()
    {
        // Resumed I/O usually rearms the same direction before the next poll.
        // Keep that subscription unchanged; remove only truly idle interests.
        for (const auto token : dirty_descriptors)
        {
            auto* found = find_descriptor(token);
            if (!found)
            {
                continue;
            }
            auto& state = *found;
            update(state, token);
            state.dirty = false;
        }
        dirty_descriptors.clear();
    }
};
} // namespace pace
