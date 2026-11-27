#pragma once

#include <pace/runtime/io_context.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace pace
{

/// A fixed set of io_contexts, each run on its own thread. Tasks stay on the context they were given to; there is no
/// work stealing. Single-use: run once, in either mode.
class runtime
{
public:
    /// Creates `workers` contexts (no threads yet). Throws std::invalid_argument for 0 workers.
    explicit runtime(
        std::size_t workers = std::max(1u, std::thread::hardware_concurrency()),
        io_backend backend = io_backend::automatic
    );
    /// Destroys the contexts. run() or run_on_current_thread() must have returned.
    ~runtime();
    runtime(const runtime&) = delete;
    runtime& operator=(const runtime&) = delete;

    /// The number of contexts.
    std::size_t size() const noexcept
    {
        return contexts_.size();
    }

    /// The context at `index`, stable for the runtime's lifetime. Throws std::out_of_range for index >= size().
    io_context& context(std::size_t index)
    {
        return *contexts_.at(index);
    }

    /// The index of the context that owns `key`: std::hash<Key> of the key, mixed and reduced modulo size(). The same
    /// key always maps to the same index for this runtime's lifetime; the mapping is not stable across processes
    /// or builds when std::hash is not. Thread-safe.
    template <class Key>
    std::size_t shard_index(const Key& key) const
    {
        auto mixed = static_cast<std::uint64_t>(std::hash<Key>{}(key));
        // splitmix64's finalizer: std::hash is the identity for integers on common standard libraries, and keys
        // such as aligned pointers or strided identifiers would otherwise land on a few contexts.
        mixed ^= mixed >> 30;
        mixed *= 0xbf58476d1ce4e5b9ULL;
        mixed ^= mixed >> 27;
        mixed *= 0x94d049bb133111ebULL;
        mixed ^= mixed >> 31;
        return static_cast<std::size_t>(mixed % contexts_.size());
    }

    /// context(shard_index(key)): the fixed owner of the state behind `key`. Thread-safe.
    template <class Key>
    io_context& shard_for(const Key& key)
    {
        return *contexts_[shard_index(key)];
    }

    /// Runs every context on its own new thread; blocks until all have stopped and been joined, then rethrows the
    /// first context error. Throws std::logic_error if the runtime has already run.
    void run();
    /// Like run(), but runs context 0 on the calling thread and the others on size() - 1 new threads. Throws
    /// std::logic_error, without using up the single run, when called on a thread already running a context.
    void run_on_current_thread();
    /// Requests stop on every context. Thread-safe, idempotent, does not wait.
    void stop() noexcept;

private:
    void run_impl(bool use_caller);
    std::vector<std::unique_ptr<io_context>> contexts_;
    std::atomic<bool> started_{false};
};

} // namespace pace
