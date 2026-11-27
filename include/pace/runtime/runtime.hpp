#pragma once

#include <pace/runtime/io_context.hpp>

#include <algorithm>
#include <atomic>
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
