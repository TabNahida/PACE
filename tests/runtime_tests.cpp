#include <pace/runtime.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <sys/eventfd.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct watchdog {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::thread thread{[this] {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, 20s, [&] { return done; })) { std::cerr << "runtime tests timed out\n"; std::abort(); }
    }};
    ~watchdog() { { std::lock_guard lock(mutex); done = true; } cv.notify_one(); thread.join(); }
};
pace::task<int> answer() { co_return 42; }
pace::task<void> fail() { throw std::runtime_error("task failure"); co_return; }
pace::task<void> basics(pace::io_context& io, int& steps) {
    require(pace::io_context::current() == &io, "current context");
    require(co_await answer() == 42, "task value");
    try { co_await fail(); require(false, "exception lost"); }
    catch (const std::runtime_error& e) { require(std::string_view(e.what()) == "task failure", "wrong exception"); }
    for (int i = 0; i < 10000; ++i) { co_await io.yield(); ++steps; }
    const auto start = std::chrono::steady_clock::now();
    co_await io.sleep_for(10ms);
    require(std::chrono::steady_clock::now() - start >= 10ms, "timer fired early");
    io.stop();
}
pace::task<void> wait_for_stop(pace::io_context& io, int& canceled) {
    try { co_await io.sleep_for(24h); }
    catch (const std::system_error& e) { require(e.code() == std::errc::operation_canceled, "timer cancellation"); ++canceled; }
}
pace::task<void> timer_stop(pace::io_context& io) { co_await io.sleep_for(5ms); io.stop(); }
pace::task<void> worker(pace::runtime& rt, std::size_t index, std::atomic<int>& count) {
    auto& io = rt.context(index);
    auto owner = std::this_thread::get_id();
    for (int i = 0; i < 1000; ++i) {
        co_await io.yield();
        require(std::this_thread::get_id() == owner, "task migrated");
        require(pace::io_context::current() == &io, "incorrect reactor");
    }
    if (++count == static_cast<int>(rt.size())) rt.stop();
}

struct pending_cleanup {
    pace::io_context* context;
    int* destroyed;
    bool* rejected;
    pending_cleanup(pace::io_context& io, int& count, bool& canceled)
        : context(&io), destroyed(&count), rejected(&canceled) {}
    pending_cleanup(pending_cleanup&& other) noexcept
        : context(std::exchange(other.context, nullptr)), destroyed(other.destroyed), rejected(other.rejected) {}
    ~pending_cleanup() {
        if (!context) return;
        ++*destroyed;
        try { context->post([] {}); }
        catch (const std::system_error& error) { *rejected = error.code() == std::errc::operation_canceled; }
    }
};
pace::task<void> never_started(pending_cleanup cleanup) { (void)cleanup; co_return; }

#if defined(__linux__)
struct owned_eventfd {
    pace::io_context& context;
    int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    explicit owned_eventfd(pace::io_context& io) : context(io) { require(fd >= 0, "eventfd creation"); }
    ~owned_eventfd() { context.cancel(fd); ::close(fd); }
};
pace::task<void> descriptor_reader(pace::io_context& io, int fd, int& count) {
    co_await io.wait_readable(fd);
    std::uint64_t value = 0;
    require(::read(fd, &value, sizeof(value)) == sizeof(value) && value == 7, "eventfd readiness result");
    ++count;
    // A delivered descriptor is disarmed, then explicitly rearmed for a new read.
    co_await io.wait_readable(fd);
    require(::read(fd, &value, sizeof(value)) == sizeof(value) && value == 11, "eventfd rearm result");
    ++count;
    io.stop();
}
pace::task<void> descriptor_writer(pace::io_context& io, int fd) {
    co_await io.sleep_for(1ms);
    std::uint64_t value = 7;
    require(::write(fd, &value, sizeof(value)) == sizeof(value), "first eventfd write");
    co_await io.sleep_for(1ms);
    value = 11;
    require(::write(fd, &value, sizeof(value)) == sizeof(value), "second eventfd write");
}
pace::task<void> canceled_descriptor_reader(pace::io_context& io, int fd, bool& canceled) {
    try { co_await io.wait_readable(fd); }
    catch (const std::system_error& error) { canceled = error.code() == std::errc::operation_canceled; }
    require(canceled, "descriptor cancellation must reach awaiter");
    io.stop();
}
pace::task<void> cancel_descriptor(pace::io_context& io, int fd) {
    co_await io.yield();
    io.cancel(fd);
}
pace::task<void> simultaneous_reader(pace::io_context& io, int fd, bool& read) {
    co_await io.wait_readable(fd);
    std::uint64_t value = 0;
    require(::read(fd, &value, sizeof(value)) == sizeof(value) && value == 19, "read after simultaneous write readiness");
    read = true;
    io.stop();
}
pace::task<void> simultaneous_writer(pace::io_context& io, int fd, bool& rejected) {
    try { co_await io.wait_readable(fd); }
    catch (const std::logic_error&) { rejected = true; }
    require(rejected, "second pending read must be rejected without removing the first");
    co_await io.wait_writable(fd);
    const std::uint64_t value = 19;
    require(::write(fd, &value, sizeof(value)) == sizeof(value), "write after write readiness");
}
#endif
void run_tests() {
    { pace::io_context io; int steps = 0; io.spawn(basics(io, steps)); io.run(); require(steps == 10000, "yield count"); }
    {
        pace::io_context io; int canceled = 0;
        for (int i = 0; i < 100; ++i) io.spawn(wait_for_stop(io, canceled));
        io.spawn(timer_stop(io)); io.run(); require(canceled == 100, "all timers drain");
    }
    {
        pace::io_context io; io.spawn(fail()); bool caught = false;
        try { io.run(); } catch (const std::runtime_error&) { caught = true; }
        require(caught, "detached failure must reach run");
    }
    {
        pace::io_context io; std::atomic<int> count{0};
        std::thread runner([&] { io.run(); });
        std::vector<std::thread> producers;
        for (int i = 0; i < 4; ++i) producers.emplace_back([&] {
            for (int n = 0; n < 2500; ++n) io.post([&] { if (++count == 10000) io.stop(); });
        });
        for (auto& producer : producers) producer.join();
        runner.join(); require(count == 10000, "cross-thread posts lost");
    }
    { pace::runtime rt(4); std::atomic<int> count{0}; for (std::size_t i=0; i<rt.size(); ++i) rt.context(i).spawn(worker(rt, i, count)); rt.run(); require(count == 4, "worker completion"); }
    {
        pace::runtime rt(2); int canceled = 0;
        rt.context(0).spawn(wait_for_stop(rt.context(0), canceled)); rt.context(1).spawn(fail());
        bool caught = false; try { rt.run(); } catch (const std::runtime_error&) { caught = true; }
        require(caught && canceled == 1, "worker failure stops peers");
    }
    {
        pace::io_context io; io.stop(); io.run();
        bool rejected = false; try { io.post([] {}); } catch (const std::system_error&) { rejected = true; }
        require(rejected, "post after stop must reject");
        rejected = false; try { io.run(); } catch (const std::logic_error&) { rejected = true; }
        require(rejected, "second run must reject");
    }
    {
        int destroyed = 0;
        bool rejected = false;
        {
            pace::io_context io;
            io.spawn(never_started(pending_cleanup(io, destroyed, rejected)));
        }
        require(destroyed == 1 && rejected, "queued task cleanup must run before context internals are destroyed");
    }
    {
        pace::io_context io;
        const auto positive = io.sleep_for(std::chrono::hours::max());
        const auto negative = io.sleep_for(std::chrono::hours::min());
        require(positive.deadline > std::chrono::steady_clock::now() + 24h, "large duration must saturate safely");
        require(negative.deadline <= std::chrono::steady_clock::now(), "negative duration must be ready immediately");
    }
#if defined(__linux__)
    {
        pace::io_context io;
        owned_eventfd descriptor(io);
        int count = 0;
        io.spawn(descriptor_reader(io, descriptor.fd, count));
        io.spawn(descriptor_writer(io, descriptor.fd));
        io.run();
        require(count == 2, "descriptor readiness and rearm");
    }
    {
        pace::io_context io;
        owned_eventfd descriptor(io);
        bool canceled = false;
        io.spawn(canceled_descriptor_reader(io, descriptor.fd, canceled));
        io.spawn(cancel_descriptor(io, descriptor.fd));
        io.run();
        require(canceled, "descriptor cancel result");
    }
    {
        pace::io_context io;
        owned_eventfd descriptor(io);
        bool read = false, rejected = false;
        io.spawn(simultaneous_reader(io, descriptor.fd, read));
        io.spawn(simultaneous_writer(io, descriptor.fd, rejected));
        io.run();
        require(read && rejected, "simultaneous read/write readiness must preserve both directions");
    }
#endif
}
}
int main() {
    watchdog timeout;
    try { run_tests(); std::cout << "runtime tests passed\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
