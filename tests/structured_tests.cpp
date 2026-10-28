#include <pace/pace.hpp>
#include <iostream>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <array>
#ifndef _WIN32
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#endif
using namespace std::chrono_literals;
void require(bool b, const char* text) { if (!b) throw std::runtime_error(text); }
struct destroyed { int& count; ~destroyed() { ++count; } };
pace::task<int> delayed(int n) { co_await pace::sleep_for(2ms); co_return n; }
pace::task<void> tests() {
    auto& io = pace::this_context();
    int finished = 0;
    auto value = co_await pace::with_task_group([&](pace::task_group& group) -> pace::task<int> {
        auto a = group.spawn(delayed(3));
        auto b = group.spawn([&]() -> pace::task<int> { destroyed done{finished}; co_await pace::sleep_for(4ms); co_return 4; });
        co_return (co_await a) + (co_await b);
    });
    require(value == 7 && finished == 1, "owned group results and joining");
    co_await pace::with_task_group([&](pace::task_group& group) -> pace::task<void> {
        (void)group.spawn([&]() -> pace::task<void> { co_await pace::sleep_for(2ms); ++finished; });
        co_return;
    });
    require(finished == 2, "ignored handle must still join");
    bool failed = false;
    try {
        co_await pace::with_task_group([&](pace::task_group& group) -> pace::task<void> {
            (void)group.spawn([&]() -> pace::task<void> { destroyed done{finished}; co_await pace::sleep_for(1h); });
            (void)group.spawn([]() -> pace::task<void> { co_await pace::yield(); throw std::runtime_error("child failure"); });
            co_return;
        });
    } catch (const std::runtime_error& e) { failed = std::string(e.what()) == "child failure"; }
    require(failed && finished == 3 && !io.stopped(), "failure cancels and drains siblings without stopping loop");
    failed = false;
    try {
        co_await pace::with_task_group([&](pace::task_group& group) -> pace::task<void> {
            (void)group.spawn([&]() -> pace::task<void> { destroyed done{finished}; co_await pace::sleep_for(1h); });
            co_await pace::yield();
            throw std::runtime_error("body failure");
        });
    } catch (const std::runtime_error& e) { failed = std::string(e.what()) == "body failure"; }
    require(failed && finished == 4, "body failure joins started children");
    bool expired = false;
    try {
        co_await pace::with_deadline(pace::deadline::after(10ms), [&]() -> pace::task<void> {
            co_await pace::with_deadline(pace::deadline::after(1h), []() -> pace::task<void> {
                co_await pace::sleep_for(1h);
            });
        });
    } catch (const std::system_error& e) { expired = e.code() == std::errc::timed_out; }
    require(expired && !io.stopped(), "nested deadlines may only shorten");
    pace::cancellation_source source;
    std::thread cancel([source] { std::this_thread::sleep_for(10ms); source.request_cancellation(); });
    bool cancelled = false;
    try { co_await pace::with_cancellation(source.token(), []() -> pace::task<void> { co_await pace::sleep_for(1h); }); }
    catch (const std::system_error& e) { cancelled = e.code() == std::errc::operation_canceled; }
    cancel.join();
    require(cancelled && !io.stopped(), "cross-thread local cancellation");
    int cleanup = 0;
    try {
        co_await pace::with_deadline(pace::deadline::after(5ms), [&]() -> pace::task<void> {
            try { co_await pace::sleep_for(1h); } catch (const std::system_error&) {}
            co_await pace::shield(pace::with_deadline(pace::deadline::after(1s), [&]() -> pace::task<void> {
                co_await pace::sleep_for(1ms); ++cleanup;
            }));
        });
    } catch (const std::system_error&) {}
    require(cleanup == 1, "shielded bounded cleanup");
    // Two independent local operations on one loop: a deadline cancels only one.
    auto receiver = pace::udp::bind({"127.0.0.1", 0});
    auto sender = pace::udp::open();
    co_await pace::with_task_group([&](pace::task_group& group) -> pace::task<void> {
        auto timed = group.spawn([&]() -> pace::task<void> {
            bool timeout = false;
            try { (void)co_await pace::with_deadline(pace::deadline::after(5ms), receiver.receive()); }
            catch (const std::system_error& e) { timeout = e.code() == std::errc::timed_out; }
            require(timeout, "UDP local timeout");
            auto packet = co_await receiver.receive();
            require(packet.data == "still alive", "cancelled UDP socket remains reusable");
        });
        auto send = group.spawn([&]() -> pace::task<void> {
            co_await pace::sleep_for(15ms);
            co_await sender.send("still alive", receiver.local_endpoint());
        });
        co_await std::move(timed); co_await std::move(send);
    });
    // The body may fail while children borrow data owned by the group itself.
    std::weak_ptr<int> lifetime;
    try {
        co_await pace::with_task_group([&](pace::task_group& group) -> pace::task<void> {
            auto& storage = group.own(std::make_shared<int>(42)); lifetime = storage;
            (void)group.spawn([&storage]() -> pace::task<void> {
                try { co_await pace::sleep_for(1h); } catch (const std::system_error&) { require(*storage == 42, "scope storage died during cancellation"); }
            });
            co_await pace::yield();
            throw std::runtime_error("scope body");
        });
    } catch (const std::runtime_error&) {}
    require(lifetime.expired(), "scope storage outlived join");
    // Nested groups drain all descendants on outer deadline expiry.
    int descendants = 0;
    try {
        co_await pace::with_task_group([&](pace::task_group& group) -> pace::task<void> {
            (void)group.spawn([&]() -> pace::task<void> {
                destroyed outer{descendants};
                co_await pace::with_task_group([&](pace::task_group& nested) -> pace::task<void> {
                    (void)nested.spawn([&]() -> pace::task<void> { destroyed inner{descendants}; co_await pace::sleep_for(1h); });
                    co_return;
                });
            });
            co_return;
        }, {.deadline=pace::deadline::after(10ms)});
    } catch (const std::system_error& e) { require(e.code()==std::errc::timed_out, "nested group timeout reason"); }
    require(descendants == 2, "nested group did not join descendants");
#ifdef PACE_HAS_DNS
    // Cancel a blackholed DNS query without shutting down the context.
    auto blackhole = pace::udp::bind({"127.0.0.1",0});
    pace::dns::options options;
    options.servers="127.0.0.1:"+std::to_string(blackhole.local_endpoint().port);
    pace::dns::resolver resolver(io, options);
    expired=false;
    try { (void)co_await pace::with_deadline(pace::deadline::after(10ms), resolver.resolve("pace.invalid")); }
    catch (const std::system_error& e) { expired=e.code()==std::errc::timed_out; }
    require(expired && !io.stopped(), "DNS local cancellation");
    require(!(co_await resolver.resolve("127.0.0.1")).addresses.empty(), "DNS resolver not reusable after cancellation");
#endif
    // Reject new children after explicit join, and complete canceled-before-start work without invoking it.
    co_await pace::with_task_group([](pace::task_group& group) -> pace::task<void> {
        co_await group.join(); bool closed = false;
        try { (void)group.spawn(delayed(1)); } catch (const std::logic_error&) { closed = true; }
        require(closed, "closed group accepts child");
    });
    bool started = false;
    source.request_cancellation();
    try { co_await pace::with_cancellation(source.token(), [&]() -> pace::task<void> { started = true; co_return; }); }
    catch (const std::system_error&) {}
    require(!started, "pre-cancelled task ran");
}
void shutdown_tests() {
    pace::io_context io;
    int drained = 0;
    bool boundary_finished = false;
    auto root = [&]() -> pace::task<void> {
        try {
            co_await pace::with_task_group([&](pace::task_group& group) -> pace::task<void> {
                (void)group.spawn([&]() -> pace::task<void> {
                    destroyed done{drained}; co_await pace::sleep_for(1h);
                });
                (void)group.spawn([&]() -> pace::task<void> {
                    destroyed done{drained}; co_await pace::yield(); io.stop();
                });
                co_return;
            });
        } catch (const std::system_error& e) {
            require(e.code() == std::errc::operation_canceled, "context stop reason");
        }
        boundary_finished = true;
        require(drained == 2, "context stop escaped before joining children");
    };
    io.spawn(root()); io.run();
    require(boundary_finished, "context stop skipped structured boundary");
}
void cancellation_callback_tests() {
    pace::io_context io;
    pace::cancellation_source source;
    int callbacks = 0;
    auto root = [&]() -> pace::task<void> {
        auto first = io.on_cancel(source.token(), [&](std::error_code) {
            ++callbacks; throw std::runtime_error("cancellation callback failure");
        });
        auto second = io.on_cancel(source.token(), [&](std::error_code) { ++callbacks; });
        source.request_cancellation();
        try { co_await io.sleep_for(1h); } catch (const std::system_error&) {}
    };
    io.spawn(root());
    bool failed = false;
    try { io.run(); } catch (const std::runtime_error& e) { failed = std::string(e.what()) == "cancellation callback failure"; }
    require(failed && callbacks == 2, "callback exception skipped remaining cancellation or draining");
}

#ifndef _WIN32
void directional_cancel_tests() {
    pace::io_context io;
    int sockets[2];
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair");
    struct cleanup {
        pace::io_context& io; int* sockets;
        ~cleanup() { for (int i=0; i<2; ++i) { io.cancel(sockets[i]); ::close(sockets[i]); } }
    } owner{io, sockets};
    for (int fd : sockets) require(::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK) == 0, "nonblocking pair");
    std::array<char, 4096> bytes{};
    while (::send(sockets[0], bytes.data(), bytes.size(), 0) > 0) {}
    require(errno == EAGAIN || errno == EWOULDBLOCK, "saturate writer");
    bool read_canceled = false, write_finished = false;
    pace::cancellation_source source;
    auto root = [&]() -> pace::task<void> {
        co_await pace::with_task_group([&](pace::task_group& group) -> pace::task<void> {
            (void)group.spawn([&]() -> pace::task<void> {
                try { co_await pace::with_cancellation(source.token(), [&]() -> pace::task<void> {
                    co_await io.wait_readable(sockets[0]);
                }); } catch (const std::system_error& e) { read_canceled = e.code() == std::errc::operation_canceled; }
            });
            (void)group.spawn([&]() -> pace::task<void> {
                co_await io.wait_writable(sockets[0]); write_finished = true;
            });
            co_await pace::sleep_for(2ms);
            source.request_cancellation();
            co_await pace::sleep_for(2ms);
            require(read_canceled && !write_finished, "read cancellation affected opposite direction");
            while (::recv(sockets[1], bytes.data(), bytes.size(), 0) > 0) {}
            co_await group.join();
            require(write_finished, "opposite direction lost readiness after local cancellation");
        }, {.deadline = pace::deadline::after(2s)});
        io.stop();
    };
    io.spawn(root()); io.run();
}
#endif

void subscription_races() {
    for (int i = 0; i < 100; ++i) {
        pace::cancellation_source source;
        std::atomic<unsigned> invoked{0};
        auto registration = std::make_unique<pace::detail::cancellation_registration>(source.token(), [&] { ++invoked; });
        std::thread request([source] { source.request_cancellation(); });
        registration.reset(); // Must synchronize with or suppress the concurrent notification.
        const auto after_removal = invoked.load();
        request.join();
        require(invoked.load() == after_removal && invoked <= 1, "notification outlived subscription");
    }
    pace::cancellation_source source;
    std::unique_ptr<pace::detail::cancellation_registration> registration;
    registration = std::make_unique<pace::detail::cancellation_registration>(source.token(), [&] { registration.reset(); });
    source.request_cancellation();
    require(!registration, "self-unregistration failed");
    pace::cancellation_source a, b;
    a.link(b.token()); b.link(a.token());
    a.request_cancellation();
    require(b.token().stop_requested(), "cyclic link propagation failed");
}

int main() {
    std::mutex mutex; std::condition_variable cv; bool done = false;
    std::thread watchdog([&] { std::unique_lock lock(mutex); if (!cv.wait_for(lock, 30s, [&] { return done; })) std::abort(); });
    int result = 0;
    try { subscription_races(); pace::run(tests); shutdown_tests(); cancellation_callback_tests();
#ifndef _WIN32
        directional_cancel_tests();
#endif
        std::cout << "Structured children, local cancellation, deadlines and cleanup passed\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; result = 1; }
    { std::lock_guard lock(mutex); done = true; } cv.notify_one(); watchdog.join();
    return result;
}
