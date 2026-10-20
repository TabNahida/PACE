#pragma once

#include <pace/task.hpp>
#include <pace/platform.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <system_error>

namespace pace {

// One reactor, one running thread. spawn/post/stop are thread-safe; I/O is affine.
class io_context {
public:
    io_context();
    ~io_context();
    io_context(const io_context&) = delete;
    io_context& operator=(const io_context&) = delete;

    void spawn(task<void> work);
    void post(std::function<void()> work);
    void run(); // Runs until stop(); unhandled task errors are rethrown after draining.
    void stop() noexcept;
    bool stopped() const noexcept;
    static io_context* current() noexcept;
    void cancel(native_socket_type fd) noexcept; // Must run on the owning loop, or before run().

    struct io_awaiter {
        io_context& context;
        native_socket_type fd;
        bool writable;
        std::error_code error;
        std::coroutine_handle<> continuation;
        bool await_ready() noexcept;
        bool await_suspend(std::coroutine_handle<> handle);
        void await_resume();
    };
    io_awaiter wait_readable(native_socket_type fd) noexcept { return {*this, fd, false, {}, {}}; }
    io_awaiter wait_writable(native_socket_type fd) noexcept { return {*this, fd, true, {}, {}}; }

    struct timer_awaiter {
        io_context& context;
        std::chrono::steady_clock::time_point deadline;
        std::error_code error;
        std::coroutine_handle<> continuation;
        bool await_ready() noexcept;
        bool await_suspend(std::coroutine_handle<> handle);
        void await_resume();
    };
    template<class Rep, class Period>
    timer_awaiter sleep_for(std::chrono::duration<Rep, Period> duration) noexcept {
        using clock = std::chrono::steady_clock;
        using ticks = std::chrono::duration<long double, clock::period>;
        const auto now = clock::now();
        const auto requested = ticks(duration).count();
        const auto maximum = static_cast<long double>(clock::time_point::max().time_since_epoch().count());
        const auto absolute = static_cast<long double>(now.time_since_epoch().count()) + requested;
        auto deadline = now;
        if (requested > 0) {
            deadline = absolute >= maximum ? clock::time_point::max()
                : clock::time_point(clock::duration(static_cast<clock::rep>(absolute)));
        }
        return {*this, deadline, {}, {}};
    }
    struct yield_awaiter {
        io_context& context;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> handle);
        void await_resume();
    };
    yield_awaiter yield() noexcept { return {*this}; }
private:
    struct implementation;
    std::unique_ptr<implementation> impl_;
    bool arm(io_awaiter&, std::coroutine_handle<>);
    bool arm(timer_awaiter&, std::coroutine_handle<>);
    void schedule(std::coroutine_handle<>);
    static void completed(void*, std::coroutine_handle<>) noexcept;
};

} // namespace pace
