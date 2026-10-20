#include <pace/pace.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;

pace::task<void> cancellable_work(pace::io_context& io, bool& canceled, bool& cleaned) {
    struct cleanup {
        bool& flag;
        ~cleanup() { flag = true; }
    } guard{cleaned};
    try {
        co_await io.sleep_for(1h);
        throw std::runtime_error("long timer unexpectedly completed");
    } catch (const std::system_error& error) {
        if (error.code() != std::errc::operation_canceled) throw;
        canceled = true;
    }
}

pace::task<void> request_stop(pace::io_context& io) {
    co_await io.sleep_for(5ms);
    io.stop();
}

int main() {
    try {
        pace::io_context io;
        bool canceled = false, cleaned = false;
        io.spawn(cancellable_work(io, canceled, cleaned));
        io.spawn(request_stop(io));
        io.run(); // Cancellation resumes the task and its local cleanup runs.
        if (!canceled || !cleaned) throw std::runtime_error("shutdown did not drain the task");
        std::cout << "Cancellation observed; coroutine resources released\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
