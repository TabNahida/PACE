#include <pace/io_context.hpp>
#include <cstdlib>
#include <iostream>
#include <new>
#include <semaphore>
#include <thread>

namespace {
thread_local bool count_allocations = false;
thread_local std::size_t allocations = 0;
}

// Diagnostic allocation count, deliberately separate from timing benchmarks.
// Count ordinary allocations made on the reactor thread between post batches.
void* operator new(std::size_t size) {
    if (auto* memory = std::malloc(size ? size : 1)) {
        if (count_allocations) ++allocations;
        return memory;
    }
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

int main() {
    constexpr std::size_t rounds = 10000;
    pace::io_context io;
    std::binary_semaphore completed(0);
    std::size_t observed = 0;
    io.post([&] { count_allocations = true; completed.release(); });
    std::thread reactor([&] { io.run(); });
    struct join_guard {
        pace::io_context& io;
        std::thread& thread;
        ~join_guard() { io.stop(); thread.join(); }
    } cleanup{io, reactor};
    completed.acquire();
    for (std::size_t round = 0; round < rounds; ++round) {
        io.post([&] { completed.release(); });
        completed.acquire();
    }
    io.post([&] {
        count_allocations = false;
        observed = allocations;
        completed.release();
    });
    completed.acquire();
    std::cout << "{\"rounds\":" << rounds << ",\"reactor_allocations\":" << observed << "}\n";
}
