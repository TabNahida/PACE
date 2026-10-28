#include <pace/runtime.hpp>
#include <pace/run.hpp>
#include <pace/detail/poller.hpp>
#include <pace/detail/profiling.hpp>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
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

void cancellable_callback_timers() {
    pace::io_context::timer_handle survivor;
    std::weak_ptr<int> released;
    {
        pace::io_context io;
        auto capture = std::make_shared<int>(42);
        released = capture;
        survivor = io.call_after(24h, [capture] {});
        capture.reset();
        require(!released.expired(), "armed timer must own its callback");
        survivor.cancel();
        require(released.expired(), "cancel must release callback immediately");
        capture = std::make_shared<int>(1);
        released = capture;
        survivor = io.call_after(24h, [capture] {});
        capture.reset();
    }
    require(released.expired(), "context destruction must release callback");
    survivor.cancel(); // An expired context must never be dereferenced.
    {
        pace::io_context io;
        std::vector<int> order;
        const auto now = std::chrono::steady_clock::now();
        auto canceled = io.call_at(now, [&] { order.push_back(99); });
        auto first = io.call_at(now, [&] { order.push_back(1); });
        auto moved = std::move(first);
        first.cancel();
        auto second = io.call_at(now, [&] { order.push_back(2); io.stop(); });
        canceled.cancel();
        io.run();
        require(order == std::vector<int>{1, 2}, "timers must fire once in deadline/registration order");
    }
    {
        pace::io_context io;
        auto other = std::make_shared<pace::io_context::timer_handle>(io.call_after(24h, [] {}));
        auto outer = io.call_after(24h, [other] {});
        other.reset();
        outer.cancel(); // Capture destruction cancels another timer reentrantly.
        bool fired = false;
        auto pending = io.call_after(0ms, [&] { fired = true; });
        io.stop(); io.run();
        require(!fired, "stop discards callbacks without firing");
    }
    {
        pace::io_context io;
        auto error = io.call_after(0ms, [] { throw std::runtime_error("callback timer failure"); });
        bool caught = false;
        try { io.run(); } catch (const std::runtime_error&) { caught = true; }
        require(caught, "timer callback errors must reach run");
    }
    {
        pace::io_context io;
        pace::io_context::timer_handle again;
        unsigned fires = 0, posts = 0;
        std::function<void()> repeat = [&] {
            ++fires;
            again = io.call_at(std::chrono::steady_clock::time_point::min(), repeat);
        };
        again = io.call_at(std::chrono::steady_clock::time_point::min(), repeat);
        std::function<void()> progress = [&] {
            if (++posts == 3) { require(fires > 0, "due callbacks must progress"); io.stop(); }
            else io.post(progress);
        };
        io.post(progress); io.run();
        require(posts == 3 && fires <= 512, "callbacks must not starve reactor work");
    }
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

pace::task<void> ready_rearm(pace::io_context& io, pace::native_socket_type fd, unsigned& resumed) {
    // Both readiness flags are set. Repeatedly reclaim both directions, then
    // each direction independently, leaving the datagram unread throughout.
    co_await io.wait_ready(fd); ++resumed;
    co_await io.sleep_for(1ms); // A duplicate queued resume would resume this too early.
    co_await io.wait_ready(fd); ++resumed;
    co_await io.wait_readable(fd); ++resumed;
    co_await io.wait_writable(fd); ++resumed;
    io.stop();
}
pace::task<void> separate_direction(pace::io_context& io, pace::native_socket_type fd,
                                    bool writable, unsigned& resumed) {
    if (writable) co_await io.wait_writable(fd);
    else co_await io.wait_readable(fd);
    if (++resumed == 2) io.stop();
}
pace::task<void> canceled_ready(pace::io_context& io, pace::native_socket_type fd, unsigned& canceled) {
    try { co_await io.wait_ready(fd); require(false, "ready wait was not canceled"); }
    catch (const std::system_error& error) {
        require(error.code() == std::errc::operation_canceled, "ready cancellation error");
        ++canceled;
    }
    io.stop();
}
pace::task<void> conflict_with_ready(pace::io_context& io, pace::native_socket_type fd, unsigned& rejected) {
    try { co_await io.wait_readable(fd); }
    catch (const std::logic_error&) { ++rejected; }
    try { co_await io.wait_writable(fd); }
    catch (const std::logic_error&) { ++rejected; }
    try { co_await io.wait_ready(fd); }
    catch (const std::logic_error&) { ++rejected; }
    require(rejected == 3, "ready wait must exclusively occupy both socket directions");
    io.cancel(fd);
}
pace::task<void> canceled_single_direction(pace::io_context& io, pace::native_socket_type fd,
                                          bool writable, unsigned& canceled) {
    try {
        if (writable) co_await io.wait_writable(fd);
        else co_await io.wait_readable(fd);
        require(false, "directional wait lost by rejected ready wait");
    } catch (const std::system_error& error) {
        require(error.code() == std::errc::operation_canceled, "directional cancellation error");
        ++canceled;
    }
    io.stop();
}
pace::task<void> reject_ready_then_cancel(pace::io_context& io, pace::native_socket_type fd, unsigned& rejected) {
    try { co_await io.wait_ready(fd); }
    catch (const std::logic_error&) { ++rejected; }
    require(rejected == 1, "ready wait must reject either occupied direction");
    io.cancel(fd);
}
pace::task<void> stop_pending_ready(pace::io_context& io) { io.stop(); co_return; }

void readiness_union_contract() {
    {
        pace::io_context io;
        datagram_pair pair;
        pair.send();
        unsigned resumed = 0;
        io.spawn(ready_rearm(io, pair.sockets[0], resumed));
        io.run();
        require(resumed == 4, "ready union must resume once and release both directions for rearm");
    }
    {
        pace::io_context io;
        datagram_pair pair;
        pair.send();
        unsigned resumed = 0;
        io.spawn(separate_direction(io, pair.sockets[0], false, resumed));
        io.spawn(separate_direction(io, pair.sockets[0], true, resumed));
        io.run();
        require(resumed == 2, "independent read/write awaiters must each resume exactly once");
    }
    {
        pace::io_context io;
        datagram_pair pair;
        unsigned canceled = 0, rejected = 0;
        io.spawn(canceled_ready(io, pair.sockets[0], canceled));
        io.spawn(conflict_with_ready(io, pair.sockets[0], rejected));
        io.run();
        require(canceled == 1 && rejected == 3, "ready cancel must resume once after rejected conflicts");
    }
    for (const bool writable : {false, true}) {
        pace::io_context io;
        datagram_pair pair;
        unsigned canceled = 0, rejected = 0;
        io.spawn(canceled_single_direction(io, pair.sockets[0], writable, canceled));
        io.spawn(reject_ready_then_cancel(io, pair.sockets[0], rejected));
        io.run();
        require(canceled == 1 && rejected == 1, "ready conflict must preserve the original awaiter");
    }
    {
        pace::io_context io;
        datagram_pair pair;
        unsigned canceled = 0;
        io.spawn(canceled_ready(io, pair.sockets[0], canceled));
        io.spawn(stop_pending_ready(io));
        io.run();
        require(canceled == 1, "context stop must cancel a readiness union only once");
    }
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

pace::task<void> record_runtime_thread(pace::runtime& rt, std::size_t index,
                                      std::vector<std::thread::id>& owners,
                                      std::atomic<std::size_t>& completed) {
    owners[index] = std::this_thread::get_id();
    auto& context = rt.context(index);
    co_await context.yield();
    require(pace::io_context::current() == &context && owners[index] == std::this_thread::get_id(),
            "runtime context migrated between execution threads");
    if (completed.fetch_add(1) + 1 == rt.size()) rt.stop();
}

void runtime_thread_modes() {
    const auto caller = std::this_thread::get_id();
    for (const bool use_caller : {false, true}) {
        for (const std::size_t count : {1U, 3U}) {
            pace::runtime rt(count);
            std::vector<std::thread::id> owners(count);
            std::atomic<std::size_t> completed{0};
            for (std::size_t i = 0; i < count; ++i)
                rt.context(i).spawn(record_runtime_thread(rt, i, owners, completed));
            if (use_caller) rt.run_on_current_thread();
            else rt.run();
            require(completed == count, "runtime did not join every context");
            require(pace::io_context::current() == nullptr, "caller retained a completed runtime context");
            for (std::size_t i = 0; i < count; ++i) {
                require((owners[i] == caller) == (use_caller && i == 0),
                        "runtime assigned a context to the wrong thread mode");
                for (std::size_t other = 0; other < i; ++other)
                    require(owners[i] != owners[other], "runtime contexts must have distinct execution threads");
            }
            bool rejected = false;
            try { rt.run(); } catch (const std::logic_error&) { rejected = true; }
            require(rejected, "runtime run must reject reuse after either mode");
            rejected = false;
            try { rt.run_on_current_thread(); } catch (const std::logic_error&) { rejected = true; }
            require(rejected, "runtime caller mode must reject reuse after either mode");
        }
    }
}

struct runtime_task_lifetime {
    unsigned& destroyed;
    ~runtime_task_lifetime() { ++destroyed; }
};

pace::task<void> runtime_waiting_peer(pace::io_context& context,
                                     std::atomic<std::size_t>& started,
                                     unsigned& canceled, unsigned& destroyed) {
    runtime_task_lifetime lifetime{destroyed};
    ++started;
    try { co_await context.sleep_for(24h); }
    catch (const std::system_error& error) {
        require(error.code() == std::errc::operation_canceled, "runtime peer shutdown error");
        ++canceled;
    }
}

pace::task<void> runtime_failing_peer(pace::runtime& rt, std::size_t index,
                                     const std::atomic<std::size_t>& started,
                                     unsigned& destroyed) {
    runtime_task_lifetime lifetime{destroyed};
    while (started.load() + 1 < rt.size()) co_await rt.context(index).yield();
    throw std::runtime_error("caller-mode peer failure");
}

void runtime_caller_failures() {
    for (const std::size_t count : {1U, 3U}) {
        for (std::size_t failing = 0; failing < count; ++failing) {
            pace::runtime rt(count);
            std::atomic<std::size_t> started{0};
            std::vector<unsigned> canceled(count), destroyed(count);
            for (std::size_t i = 0; i < count; ++i) {
                if (i == failing)
                    rt.context(i).spawn(runtime_failing_peer(rt, i, started, destroyed[i]));
                else rt.context(i).spawn(runtime_waiting_peer(rt.context(i), started, canceled[i], destroyed[i]));
            }
            bool propagated = false;
            try { rt.run_on_current_thread(); }
            catch (const std::runtime_error& error) {
                propagated = std::string_view(error.what()) == "caller-mode peer failure";
            }
            require(propagated, "caller or worker failure must propagate through caller mode");
            require(pace::io_context::current() == nullptr, "failed caller mode retained its context");
            for (std::size_t i = 0; i < count; ++i) {
                require(rt.context(i).stopped() && destroyed[i] == 1,
                        "caller mode must stop, drain and destroy every context task before rethrowing");
                require(canceled[i] == (i == failing ? 0U : 1U), "runtime failure did not cancel a peer exactly once");
            }
        }
    }
}

void runtime_nested_caller_rejection() {
    pace::runtime rt(2);
    pace::io_context outer;
    outer.post([&] {
        bool rejected = false;
        try { rt.run_on_current_thread(); } catch (const std::logic_error&) { rejected = true; }
        require(rejected && pace::io_context::current() == &outer,
                "caller mode must reject nesting without replacing the active context");
        outer.stop();
    });
    outer.run();
    std::atomic<int> completed{0};
    for (std::size_t i = 0; i < rt.size(); ++i)
        rt.context(i).post([&] { if (++completed == 2) rt.stop(); });
    rt.run_on_current_thread();
    require(completed == 2, "a rejected nested caller attempt must leave the runtime usable");
}

void runtime_concurrent_run_rejection() {
    pace::runtime rt(1);
    completion_signal started;
    std::exception_ptr failure;
    rt.context(0).post([&] { started.arrive(); });
    std::thread runner([&] {
        try { rt.run_on_current_thread(); }
        catch (...) { failure = std::current_exception(); }
    });
    struct join_guard {
        pace::runtime& rt;
        std::thread& runner;
        ~join_guard() { rt.stop(); if (runner.joinable()) runner.join(); }
    } cleanup{rt, runner};
    started.wait_for(1);
    for (const bool use_caller : {false, true}) {
        bool rejected = false;
        try { if (use_caller) rt.run_on_current_thread(); else rt.run(); }
        catch (const std::logic_error&) { rejected = true; }
        require(rejected && !rt.context(0).stopped(),
                "concurrent run rejection must not stop the active runtime");
    }
    rt.stop();
    runner.join();
    if (failure) std::rethrow_exception(failure);
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

pace::task<void> ready_one_direction(pace::io_context& io, int fd, unsigned& resumed) {
    co_await io.wait_ready(fd);
    ++resumed;
    io.cancel(fd);
    io.stop();
}

void ready_single_flag_contract() {
    // An empty eventfd is only writable; a saturated one is only readable.
    // Whichever flag delivers the union, cancellation must not find its other
    // slot still pointing into the already-resumed coroutine.
    for (const auto value : {UINT64_C(0), UINT64_MAX - UINT64_C(1)}) {
        pace::io_context io;
        owned_eventfd descriptor(io);
        if (value) require(::write(descriptor.fd, &value, sizeof(value)) == sizeof(value), "saturate eventfd");
        unsigned resumed = 0;
        io.spawn(ready_one_direction(io, descriptor.fd, resumed));
        io.run();
        require(resumed == 1, "single readiness flag must resume and clear a union once");
    }
}

void persistent_poller_contract() {
    pace::io_context owner;
    owned_eventfd descriptor(owner);
    pace::detail::poller poller(true);
    std::array<pace::detail::poll_event, 1> events{};
    const std::uint64_t value = 1;
    require(::write(descriptor.fd, &value, sizeof(value)) == sizeof(value), "persistent eventfd write");
    poller.update(descriptor.fd, true, false, 41);
    for (unsigned i = 0; i < 2; ++i)
        require(poller.wait(events, 0) == 1 && events[0].token == 41 && events[0].readable,
                "persistent readiness must remain subscribed without rearm");
    poller.update(descriptor.fd, false, true, 42);
    require(poller.wait(events, 0) == 1 && events[0].token == 42 && events[0].writable && !events[0].readable,
            "persistent interest change must replace the old direction and token");
    poller.update(descriptor.fd, false, false, 43);
    require(poller.wait(events, 0) == 0, "idle persistent descriptor must be removed");
}

pace::task<void> batched_descriptor_reader(pace::io_context& io, int fd, std::size_t index,
                                          std::size_t total, std::size_t& completed) {
    co_await io.wait_readable(fd);
    // Leave the descriptor readable while some tasks suspend on a different
    // primitive. Its retained kernel interest must not resume this task twice.
    if (index % 2) co_await io.sleep_for(1ms);
    co_await io.wait_readable(fd);
    std::uint64_t value = 0;
    require(::read(fd, &value, sizeof(value)) == sizeof(value) && value == index + 1,
            "batched readiness was lost, duplicated or assigned to another descriptor");
    io.cancel(fd);
    if (++completed == total) io.stop();
}

void persistent_reactor_batches() {
    pace::io_context io;
    // Exceed both the poll event capacity and the bounded ready dispatch batch.
    constexpr std::size_t count = 600;
    std::vector<std::unique_ptr<owned_eventfd>> descriptors;
    std::size_t completed = 0;
    for (std::size_t i = 0; i < count; ++i) {
        descriptors.push_back(std::make_unique<owned_eventfd>(io));
        const std::uint64_t value = i + 1;
        const int fd = descriptors.back()->fd;
        require(::write(fd, &value, sizeof(value)) == sizeof(value), "batched eventfd write");
        io.spawn(batched_descriptor_reader(io, fd, i, count, completed));
    }
    io.run();
    require(completed == count, "all batched descriptors must complete exactly once");
}

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

pace::task<void> entry_background(pace::io_context& io, bool& canceled) {
    try { co_await io.sleep_for(1h); }
    catch (const std::system_error& error) {
        if (error.code() != std::errc::operation_canceled) throw;
        canceled = true;
    }
}
pace::task<void> entry_background_failure() {
    throw std::runtime_error("background failure");
    co_return;
}
pace::task<int> context_free_entry() {
    co_await pace::sleep_for(1ms);
    co_await pace::yield();
    require(&pace::this_context() == pace::io_context::current(), "current context mismatch");
    co_return 17;
}
void entry_runner_contract() {
    bool no_context = false;
    try { (void)pace::sleep_for(1ms); } catch (const std::logic_error&) { no_context = true; }
    require(no_context, "context-free timer created a hidden event loop");
    require(pace::run(context_free_entry()) == 17, "direct task runner lost result");
    require(pace::run(context_free_entry) == 17, "nullary factory runner lost result");
    require(pace::run([value = std::make_unique<int>(23)]() -> pace::task<int> {
        co_await pace::yield(); co_return *value;
    }) == 23, "nullary capturing factory lost lifetime");
    const auto caller = std::this_thread::get_id();
    auto result = pace::run([owned = std::make_unique<int>(42), caller](pace::io_context& io) mutable
                           -> pace::task<std::unique_ptr<int>> {
        require(pace::io_context::current() == &io && std::this_thread::get_id() == caller,
                "entry factory did not execute on caller's reactor");
        co_await io.yield();
        co_return std::move(owned);
    });
    require(result && *result == 42, "entry result or move-only closure lifetime lost");
    bool canceled = false;
    pace::run([&canceled](pace::io_context& io) -> pace::task<void> {
        io.spawn(entry_background(io, canceled));
        co_await io.yield(); // Let the background root enter its timer.
    });
    require(canceled, "entry return failed to drain background cancellation");
    bool nested_called = false;
    pace::run([&](pace::io_context&) -> pace::task<void> {
        bool rejected = false;
        try { pace::run([&](pace::io_context&) -> pace::task<void> { nested_called = true; co_return; }); }
        catch (const std::logic_error&) { rejected = true; }
        require(rejected && !nested_called, "nested run invoked the factory");
        co_return;
    });
    bool failure = false;
    try {
        pace::run([](pace::io_context& io) -> pace::task<void> {
            co_await io.yield(); throw std::runtime_error("entry failure");
        });
    } catch (const std::runtime_error& error) { failure = std::string_view(error.what()) == "entry failure"; }
    require(failure, "entry exception was lost");
    failure = false;
    try {
        pace::run([](pace::io_context&) -> pace::task<void> { throw std::runtime_error("factory failure"); });
    } catch (const std::runtime_error& error) { failure = std::string_view(error.what()) == "factory failure"; }
    require(failure, "synchronous factory exception was lost");
    failure = false;
    try {
        pace::run([](pace::io_context& io) -> pace::task<int> {
            io.stop(); co_await io.sleep_for(1ms); co_return 1;
        });
    } catch (const std::system_error& error) { failure = error.code() == std::errc::operation_canceled; }
    require(failure, "entry cancellation silently returned a missing result");
    failure = false;
    try {
        pace::run([](pace::io_context& io) -> pace::task<int> {
            io.stop(); co_await std::suspend_always{}; co_return 1;
        });
    } catch (const std::system_error& error) { failure = error.code() == std::errc::operation_canceled; }
    require(failure, "incomplete entry accessed an absent result");
    failure = false;
    try {
        pace::run([](pace::io_context& io) -> pace::task<void> {
            io.spawn(entry_background_failure()); co_await io.sleep_for(1h);
        });
    } catch (const std::runtime_error& error) { failure = std::string_view(error.what()) == "background failure"; }
    require(failure, "background exception was swallowed by entry shutdown");
    failure = false;
    try { pace::run([](pace::io_context&) { return pace::task<void>{}; }); }
    catch (const std::logic_error&) { failure = true; }
    require(failure && pace::io_context::current() == nullptr, "empty entry or current context cleanup");
}

void run_tests() {
    entry_runner_contract();
    profiling_contract();
    poller_output_contract();
    readiness_union_contract();
    mixed_concurrent_submissions();
    repeated_sleep_and_wake();
    timer_fairness_with_ready_work();
    cancellable_callback_timers();
    shutdown_exactly_once();
    runtime_thread_modes();
    runtime_caller_failures();
    runtime_nested_caller_rejection();
    runtime_concurrent_run_rejection();
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
    ready_single_flag_contract();
    persistent_poller_contract();
    persistent_reactor_batches();
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
