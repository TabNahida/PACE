#include <pace/pace.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

using namespace std::chrono_literals;

pace::task<std::string> delayed_message(pace::io_context& io, std::string name) {
    co_await io.sleep_for(5ms);
    co_return "Hello, " + name;
}

pace::task<void> expected_failure() {
    throw std::runtime_error("child task failed");
    co_return;
}

pace::task<void> lesson(pace::io_context& io) {
    // The string is owned by the child frame. The context remains borrowed.
    auto pending = delayed_message(io, "PACE");
    const auto message = co_await std::move(pending);
    if (message != "Hello, PACE" || pace::io_context::current() != &io)
        throw std::runtime_error("task result or execution context mismatch");
    std::cout << message << '\n';
    co_await io.yield();
    co_await io.sleep_for(0ms); // Even an immediately due timer yields.
    bool caught = false;
    try { co_await expected_failure(); }
    catch (const std::runtime_error&) { caught = true; }
    if (!caught) throw std::runtime_error("child exception was lost");
    io.stop();
}

int main() {
    try {
        pace::io_context io;
        io.spawn(lesson(io));
        io.run(); // This thread executes the coroutine; no runtime is required.
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
