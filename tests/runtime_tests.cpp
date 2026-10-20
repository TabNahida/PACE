#include <pace/runtime.hpp>
#include <pace/detail/poller.hpp>
#include <pace/detail/profiling.hpp>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#if !defined(_WIN32)
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <sys/eventfd.h>
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

struct completion_signal {
    std::mutex mutex;
    std::condition_variable condition;
    std::size_t completed = 0;
    void arrive() {
        { std::lock_guard lock(mutex); ++completed; }
        condition.notify_one();
    }
    void wait_for(std::size_t target) {
        std::unique_lock lock(mutex);
        require(condition.wait_for(lock, 2s, [&] { return completed >= target; }),
                "submission did not wake and complete on its reactor");
    }
};

struct loop_thread {
    pace::io_context& context;
    std::exception_ptr failure;
    std::thread thread;
    explicit loop_thread(pace::io_context& io) : context(io), thread([this] {
        try { context.run(); }
        catch (...) { failure = std::current_exception(); }
    }) {}
    ~loop_thread() { context.stop(); if (thread.joinable()) thread.join(); }
    void finish() {
        context.stop();
        thread.join();
        if (failure) std::rethrow_exception(failure);
    }
};

struct thread_group {
    std::vector<std::thread> threads;
    ~thread_group() { join(); }
    void join() { for (auto& thread : threads) if (thread.joinable()) thread.join(); }
};

pace::task<void> mixed_task(pace::io_context& io, std::vector<unsigned>& visits,
                            std::size_t id, completion_signal& completed) {
    require(pace::io_context::current() == &io, "spawn resumed on the wrong reactor");
    co_await io.yield();
    ++visits[id];
    completed.arrive();
}

void mixed_concurrent_submissions() {
    pace::io_context io;
    constexpr std::size_t producers = 4, per_producer = 64;
    std::vector<unsigned> visits(producers * per_producer * 2);
    completion_signal completed;
    loop_thread runner(io);
    std::mutex failure_mutex;
    std::exception_ptr failure;
    thread_group submissions;
    for (std::size_t producer = 0; producer < producers; ++producer) {
        submissions.threads.emplace_back([&, producer] {
            try {
                for (std::size_t n = 0; n < per_producer; ++n) {
                    const auto id = (producer * per_producer + n) * 2;
                    auto submit_post = [&] {
                        io.post([&, id] {
                            require(pace::io_context::current() == &io, "post ran on the wrong reactor");
                            ++visits[id];
                            completed.arrive();
                        });
                    };
                    // Exercise both incoming queues becoming nonempty first.
                    if ((producer + n) % 2 == 0) submit_post();
                    io.spawn(mixed_task(io, visits, id + 1, completed));
                    if ((producer + n) % 2 != 0) submit_post();
                }
            } catch (...) {
                std::lock_guard lock(failure_mutex);
                if (!failure) failure = std::current_exception();
            }
        });
    }
    submissions.join();
    if (failure) std::rethrow_exception(failure);
    completed.wait_for(visits.size());
    runner.finish();
    for (const auto count : visits) require(count == 1, "mixed submission was lost or executed twice");
}

pace::task<void> wake_round(pace::io_context& io, completion_signal& completed) {
    co_await io.sleep_for(1ms);
    completed.arrive();
}

void repeated_sleep_and_wake() {
    pace::io_context io;
    completion_signal completed;
    loop_thread runner(io);
    for (std::size_t round = 0; round < 24; ++round) {
        // Let the preceding inbox drain before creating the next wakeup.
        // This is a liveness test; no scheduling latency is asserted.
        std::this_thread::sleep_for(1ms);
        if (round % 2 == 0) io.spawn(wake_round(io, completed));
        else io.post([&] { io.spawn(wake_round(io, completed)); });
        completed.wait_for(round + 1);
    }
    runner.finish();
    require(completed.completed == 24, "sleep/wake round executed more than once");
}

pace::task<void> due_timer(pace::io_context& io, bool& fired) {
    co_await io.sleep_for(0ms);
    fired = true;
}

pace::task<void> continuously_ready(pace::io_context& io, const bool& timer_fired,
                                    unsigned& yields) {
    while (!timer_fired && yields < 4096) {
        ++yields;
        co_await io.yield();
    }
    require(timer_fired, "ready coroutines starved a due timer");
    io.stop();
}

void timer_fairness_with_ready_work() {
    pace::io_context io;
    bool timer_fired = false;
    unsigned yields = 0, posts = 0;
    std::function<void()> ready_post = [&] {
        if (timer_fired) return;
        require(++posts < 4096, "ready callbacks starved a due timer");
        io.post(ready_post);
    };
    io.post(ready_post);
    io.spawn(due_timer(io, timer_fired));
    io.spawn(continuously_ready(io, timer_fired, yields));
    io.run();
    // Both queues remain ready until the timer runs. Bound by dispatch turns,
    // not wall-clock latency, so slow or oversubscribed CI machines still pass.
    require(timer_fired && yields > 0 && yields < 4096 && posts > 0 && posts < 4096,
            "due timer must make progress while callbacks and coroutines are ready");
}

pace::task<void> counted_cancellation(pace::io_context& io, unsigned& canceled,
                                      unsigned& destroyed, completion_signal& started) {
    struct lifetime {
        unsigned& count;
        ~lifetime() { ++count; }
    } cleanup{destroyed};
    started.arrive();
    try { co_await io.sleep_for(24h); }
    catch (const std::system_error& error) {
        require(error.code() == std::errc::operation_canceled, "unexpected shutdown error");
        ++canceled;
    }
}

void shutdown_exactly_once() {
    pace::io_context io;
    std::array<unsigned, 32> canceled{}, destroyed{};
    std::array<unsigned, 300> posted{};
    completion_signal started;
    for (std::size_t i = 0; i < canceled.size(); ++i)
        io.spawn(counted_cancellation(io, canceled[i], destroyed[i], started));
    for (auto& count : posted) io.post([&count] { ++count; });
    loop_thread runner(io);
    started.wait_for(canceled.size());
    thread_group stopping;
    for (unsigned i = 0; i < 3; ++i)
        stopping.threads.emplace_back([&] { for (unsigned n = 0; n < 8; ++n) io.stop(); });
    stopping.join();
    runner.finish();
    for (std::size_t i = 0; i < canceled.size(); ++i)
        require(canceled[i] == 1 && destroyed[i] == 1, "shutdown must resume and destroy each task once");
    for (const auto count : posted) require(count == 1, "shutdown lost or repeated an accepted post");

    // Stopping before run must also drain every already accepted callback.
    pace::io_context stopped;
    unsigned drained = 0;
    for (unsigned i = 0; i < 300; ++i) stopped.post([&] { ++drained; });
    stopped.stop();
    stopped.stop();
    stopped.run();
    require(drained == 300, "stop before run must drain callbacks across batch boundaries");
}

struct datagram_pair {
    std::array<pace::native_socket_type, 2> sockets{pace::invalid_socket, pace::invalid_socket};
    datagram_pair() {
        pace::detail::initialize_sockets();
        try {
#if !defined(_WIN32)
            // Local socket pairs avoid port allocation and external networking.
            require(::socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets.data()) == 0,
                    "poller test socket pair creation");
            for (const auto socket : sockets) {
                const int flags = ::fcntl(socket, F_GETFL, 0);
                require(flags >= 0 && ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0,
                        "poller test nonblocking mode");
            }
#else
            std::array<sockaddr_in, 2> addresses{};
            for (std::size_t i = 0; i < sockets.size(); ++i) {
                sockets[i] = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                require(sockets[i] != pace::invalid_socket, "poller test socket creation");
                addresses[i].sin_family = AF_INET;
                addresses[i].sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                require(::bind(sockets[i], reinterpret_cast<const sockaddr*>(&addresses[i]),
                               sizeof(addresses[i])) == 0, "poller test bind");
                int length = sizeof(addresses[i]);
                u_long nonblocking = 1;
                require(::ioctlsocket(sockets[i], FIONBIO, &nonblocking) == 0, "poller test nonblocking mode");
                require(::getsockname(sockets[i], reinterpret_cast<sockaddr*>(&addresses[i]), &length) == 0,
                        "poller test local endpoint");
            }
            for (std::size_t i = 0; i < sockets.size(); ++i)
                require(::connect(sockets[i], reinterpret_cast<const sockaddr*>(&addresses[1 - i]),
                                  sizeof(addresses[0])) == 0, "poller test UDP connect");
#endif
        } catch (...) { close(); throw; }
    }
    datagram_pair(const datagram_pair&) = delete;
    ~datagram_pair() { close(); }
    void close() noexcept {
        for (auto& socket : sockets) {
            if (socket == pace::invalid_socket) continue;
#if defined(_WIN32)
            ::closesocket(socket);
#else
            ::close(socket);
#endif
            socket = pace::invalid_socket;
        }
    }
    void send() {
        const char value = 7;
#if defined(_WIN32)
        require(::send(sockets[1], &value, 1, 0) == 1, "poller test send");
#else
        require(::write(sockets[1], &value, 1) == 1, "poller test write");
#endif
    }
};

void poller_output_contract() {
    using pace::detail::poll_event;
    pace::detail::poller poller;
    constexpr poll_event sentinel{9000, false, true, true};
    std::array<poll_event, 4> output;
    output.fill(sentinel);
    require(poller.wait({}, 0) == 0, "empty poll output span");
    require(poller.wait(output, 0) == 0, "empty poll unexpectedly returned an event");
    poller.wake();
    require(poller.wait(output, 1000) == 0, "internal wakeup must not escape as a socket event");
    require(output[0].token == sentinel.token, "empty poll overwrote output");

    std::array<datagram_pair, 3> pairs;
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        pairs[i].send();
        poller.update(pairs[i].sockets[0], true, false, 100 + i);
    }
    std::array<unsigned, 3> delivered{};
    std::size_t total = 0;
    while (total < pairs.size()) {
        output.fill(sentinel);
        const auto count = poller.wait(std::span<poll_event>(output).first(2), 1000);
        require(count > 0 && count <= 2, "poller lost a ready descriptor or exceeded its output span");
        require(output[2].token == sentinel.token && output[3].token == sentinel.token,
                "poller wrote beyond the supplied output span");
        for (std::size_t i = 0; i < count; ++i) {
            const auto& event = output[i];
            require(event.token >= 100 && event.token < 103, "poller returned an unknown token");
            require(event.readable && !event.writable && !event.error, "poller read flags were not fully assigned");
            require(++delivered[static_cast<std::size_t>(event.token - 100)] == 1,
                    "descriptor was delivered twice without rearming");
        }
        total += count;
    }
    require(poller.wait(output, 0) == 0, "one-shot delivery must disarm unread descriptors");

    const auto fd = pairs[0].sockets[0];
    poller.update(fd, true, false, 201);
    poller.update(fd, true, false, 202);
    output.fill(sentinel);
    require(poller.wait(output, 1000) == 1 && output[0].token == 202 && output[0].readable &&
            !output[0].writable && !output[0].error, "rearm must preserve the latest token and overwrite all flags");
    poller.update(fd, true, true, 203);
    require(poller.wait(output, 1000) == 1 && output[0].token == 203 && output[0].readable &&
            output[0].writable && !output[0].error, "simultaneous read/write must produce one complete event");
    require(poller.wait(output, 0) == 0, "both directions must be disarmed after delivery");
    poller.update(fd, true, false, 204);
    poller.remove(fd);
    require(poller.wait(output, 0) == 0, "removed descriptor was delivered");
    poller.update(fd, true, false, 205);
    poller.update(fd, false, false, 206);
    require(poller.wait(output, 0) == 0, "empty interest must remove the descriptor");
    for (const auto& pair : pairs) poller.remove(pair.sockets[0]);
}

void profiling_contract() {
#ifdef PACE_ENABLE_PROFILING
    namespace profile = pace::detail::profiling;
    profile::reset();
#ifdef _WIN32
    pace::detail::initialize_sockets();
#endif
    errno = EAGAIN;
#ifdef _WIN32
    ::WSASetLastError(WSAEWOULDBLOCK);
#endif
    {
        profile::scope outer(profile::stage::coroutine_execution);
        require(errno == EAGAIN, "profiling constructor changed errno");
#ifdef _WIN32
        require(::WSAGetLastError() == WSAEWOULDBLOCK, "profiling constructor changed Winsock error");
#endif
        {
            profile::scope child(profile::stage::recv);
            { profile::scope grandchild(profile::stage::send); }
            child.close();
            child.close();
            require(profile::snapshot()[static_cast<std::size_t>(profile::stage::recv)].calls == 1,
                    "explicit profiling close must be idempotent");
        }
        errno = EINTR;
#ifdef _WIN32
        ::WSASetLastError(WSAENOTCONN);
#endif
        outer.close();
        require(errno == EINTR, "profiling close changed errno");
#ifdef _WIN32
        require(::WSAGetLastError() == WSAENOTCONN, "profiling close changed Winsock error");
#endif
    }
    const auto values = profile::snapshot();
    const auto outer = values[static_cast<std::size_t>(profile::stage::coroutine_execution)];
    const auto child = values[static_cast<std::size_t>(profile::stage::recv)];
    const auto grandchild = values[static_cast<std::size_t>(profile::stage::send)];
    require(outer.calls == 1 && child.calls == 1 && grandchild.calls == 1,
            "scope destruction must not count an explicitly closed scope twice");
    require(outer.inclusive_ns == outer.exclusive_ns + child.inclusive_ns &&
            child.inclusive_ns == child.exclusive_ns + grandchild.inclusive_ns &&
            grandchild.inclusive_ns == grandchild.exclusive_ns,
            "nested profiling must conserve exclusive and inclusive time");
    require(profile::active == nullptr, "profiling scope stack did not unwind");
    bool propagated = false;
    try {
        profile::measure(profile::stage::accept, [] {
            errno = EINTR;
            throw std::runtime_error("profiled operation failed");
        });
    } catch (const std::runtime_error&) { propagated = true; }
    require(propagated && profile::active == nullptr,
            "profiling must unwind its stack when an operation throws");
    require(profile::snapshot()[static_cast<std::size_t>(profile::stage::accept)].calls == 1,
            "throwing profiled operation must be counted once");
    {
        errno = EAGAIN;
#ifdef _WIN32
        ::WSASetLastError(WSAEWOULDBLOCK);
#endif
        {
            profile::preserve_error saved;
            errno = EINTR;
#ifdef _WIN32
            ::WSASetLastError(WSAENOTCONN);
#endif
        }
        require(errno == EAGAIN, "profiling error guard did not restore errno");
#ifdef _WIN32
        require(::WSAGetLastError() == WSAEWOULDBLOCK, "profiling error guard did not restore Winsock error");
#endif
    }
    profile::reset();
#endif
}

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
    profiling_contract();
    poller_output_contract();
    mixed_concurrent_submissions();
    repeated_sleep_and_wake();
    timer_fairness_with_ready_work();
    shutdown_exactly_once();
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
