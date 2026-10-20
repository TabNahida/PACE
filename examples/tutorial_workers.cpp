#include <pace/pace.hpp>

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

pace::task<void> worker_task(pace::runtime& runtime, std::size_t index,
                             std::atomic<std::size_t>& completed,
                             std::thread::id caller, bool caller_mode) {
    auto& io = runtime.context(index);
    if (pace::io_context::current() != &io)
        throw std::runtime_error("task migrated to the wrong context");
    if (caller_mode && index == 0 && std::this_thread::get_id() != caller)
        throw std::runtime_error("context zero did not execute on the caller");
    co_await io.yield();
    if (completed.fetch_add(1, std::memory_order_acq_rel) + 1 == runtime.size())
        runtime.stop();
}

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc > 1 ? argv[1] : "caller";
        if (argc > 2 || (mode != "caller" && mode != "background"))
            throw std::invalid_argument("usage: tutorial_workers [caller|background]");
        pace::runtime runtime(2); // Explicit total reactor count, including caller in caller mode.
        std::atomic<std::size_t> completed{0};
        const auto caller = std::this_thread::get_id();
        for (std::size_t index = 0; index < runtime.size(); ++index)
            runtime.context(index).spawn(worker_task(runtime, index, completed, caller, mode == "caller"));
        if (mode == "caller") runtime.run_on_current_thread();
        else runtime.run();
        if (completed.load() != runtime.size()) throw std::runtime_error("worker did not finish");
        std::cout << "Two contexts completed in " << mode << " mode\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
