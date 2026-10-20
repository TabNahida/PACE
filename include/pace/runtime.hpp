#pragma once

#include <pace/io_context.hpp>
#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

namespace pace {

// Sharded reactors: tasks never migrate after being assigned to a context.
class runtime {
public:
    explicit runtime(std::size_t workers = std::max(1u, std::thread::hardware_concurrency()));
    ~runtime();
    runtime(const runtime&) = delete;
    runtime& operator=(const runtime&) = delete;
    std::size_t size() const noexcept { return contexts_.size(); }
    io_context& context(std::size_t index) { return *contexts_.at(index); }
    // Both entry points block until stopped, join all workers, and rethrow the
    // first context error. A runtime may be run only once across both modes.
    void run();
    // Run context(0) on this caller and the remaining contexts on N-1 workers.
    // With one context no worker thread is created. Rejects calls from inside
    // another running io_context without consuming this runtime's single run.
    void run_on_current_thread();
    void stop() noexcept;
private:
    void run_impl(bool use_caller);
    std::vector<std::unique_ptr<io_context>> contexts_;
    std::atomic<bool> started_{false};
};

} // namespace pace
