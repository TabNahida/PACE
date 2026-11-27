#pragma once
#include <pace/runtime/runtime.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pace
{
/// Publishes an immutable value to a fixed set of contexts, RCU style. Each context reads its own cached
/// std::shared_ptr<const T>: no lock, no atomic read-modify-write, no reference count change. publish() stores the
/// new value and asks every context to refresh its copy on its next turn; it never waits for readers, and readers
/// never wait for it. A snapshot is a handle: copies share the value and the caches.
template <class T>
class snapshot
{
    struct slot
    {
        remote_access::anchor* identity = nullptr;
        std::shared_ptr<remote_access::anchor> anchor;
        std::shared_ptr<const T> cached; ///< Read and written only on its context's thread once shared.
        std::atomic<bool> pending{false};
    };

    struct state
    {
        std::mutex mutex;
        std::shared_ptr<const T> latest;
        std::unique_ptr<slot[]> slots;
        std::size_t size = 0;

        void refresh(std::size_t index)
        {
            auto& entry = slots[index];
            // Cleared before reading `latest`: a publish after this point sees no pending refresh and posts another.
            entry.pending.store(false);
            std::shared_ptr<const T> current;
            {
                std::scoped_lock lock(mutex);
                current = latest;
            }
            entry.cached.swap(current); // The previous value is released outside the lock.
        }
    };

    std::shared_ptr<state> state_;

    static std::shared_ptr<state> make(const std::vector<io_context*>& contexts, std::shared_ptr<const T> initial)
    {
        if (contexts.empty())
        {
            throw std::invalid_argument("snapshot needs at least one context");
        }
        auto created = std::make_shared<state>();
        created->slots = std::make_unique<slot[]>(contexts.size());
        created->size = contexts.size();
        for (std::size_t i = 0; i < contexts.size(); ++i)
        {
            if (!contexts[i])
            {
                throw std::invalid_argument("snapshot context is null");
            }
            for (std::size_t j = 0; j < i; ++j)
            {
                if (contexts[j] == contexts[i])
                {
                    throw std::invalid_argument("snapshot context listed twice");
                }
            }
            auto& entry = created->slots[i];
            entry.anchor = remote_access::anchor_of(*contexts[i]);
            entry.identity = entry.anchor.get();
            entry.cached = initial;
        }
        created->latest = std::move(initial);
        return created;
    }

    static std::vector<io_context*> contexts_of(runtime& workers)
    {
        std::vector<io_context*> contexts;
        contexts.reserve(workers.size());
        for (std::size_t i = 0; i < workers.size(); ++i)
        {
            contexts.push_back(&workers.context(i));
        }
        return contexts;
    }

public:
    /// Publishes `initial` to every context of `workers`. Thread-safe; the contexts need not be running yet.
    explicit snapshot(runtime& workers, std::shared_ptr<const T> initial = nullptr)
        : state_(make(contexts_of(workers), std::move(initial)))
    {
    }

    /// Publishes `initial` to each of `contexts`. Throws std::invalid_argument for an empty list, a null or a
    /// repeated context.
    explicit snapshot(const std::vector<io_context*>& contexts, std::shared_ptr<const T> initial = nullptr)
        : state_(make(contexts, std::move(initial)))
    {
    }

    /// Shares the value and the caches.
    snapshot(const snapshot&) = default;
    /// Shares `other`'s value and caches.
    snapshot& operator=(const snapshot&) = default;
    /// Takes the handle; a moved-from snapshot may only be assigned or destroyed.
    snapshot(snapshot&&) noexcept = default;
    /// Takes the handle; a moved-from snapshot may only be assigned or destroyed.
    snapshot& operator=(snapshot&&) noexcept = default;
    /// Releases this handle; the values go with the last one and with pending refreshes.
    ~snapshot() = default;

    /// The calling context's copy: the value published last as of this context's latest refresh, which may trail
    /// a publish from another thread by one turn of this context. The reference stays valid until the calling
    /// coroutine next suspends; copy the shared_ptr to keep the value longer. Linear in the number of contexts.
    /// Throws std::logic_error on a thread that runs none of the snapshot's contexts.
    const std::shared_ptr<const T>& get() const
    {
        const auto* here = remote_access::current();
        const auto& s = *state_;
        for (std::size_t i = 0; i < s.size; ++i)
        {
            if (s.slots[i].identity == here && here)
            {
                return s.slots[i].cached;
            }
        }
        throw std::logic_error("snapshot read outside its contexts");
    }

    /// The value published last, from any thread. Takes the publisher lock; prefer get() on a context.
    std::shared_ptr<const T> load() const
    {
        std::scoped_lock lock(state_->mutex);
        return state_->latest;
    }

    /// Replaces the value, from any thread. The calling context (if it is one of the snapshot's) sees it at once,
    /// the others on their next turn. Several quick publishes may reach a context as one refresh to the latest.
    /// Stopped contexts are skipped. May throw std::bad_alloc after storing the value.
    void publish(std::shared_ptr<const T> value)
    {
        auto& s = *state_;
        {
            std::scoped_lock lock(s.mutex);
            s.latest.swap(value);
        }
        value.reset(); // The previous value is released outside the lock.
        const auto* here = remote_access::current();
        for (std::size_t i = 0; i < s.size; ++i)
        {
            auto& entry = s.slots[i];
            if (entry.identity == here)
            {
                s.refresh(i);
            }
            else if (!entry.pending.exchange(true))
            {
                // Not to a stopped context: a stream of publishes must not keep its drain from finishing.
                if (!remote_access::post(*entry.anchor, [shared = state_, i] { shared->refresh(i); }, false))
                {
                    entry.pending.store(false);
                }
            }
        }
    }
};
} // namespace pace
