#pragma once

#include <pace/io_context.hpp>
#include <algorithm>
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
    void run();
    void stop() noexcept;
private:
    std::vector<std::unique_ptr<io_context>> contexts_;
};

} // namespace pace
