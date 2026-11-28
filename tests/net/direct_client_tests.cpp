#include <pace/pace.hpp>
#include "support/pace_test.hpp"

#include <chrono>
#include <filesystem>
#include <thread>
#include <type_traits>

namespace
{
using namespace std::chrono_literals;

using pace::test::require;

template <class Socket>
struct peer
{
    pace::io_context io;
    Socket socket{io, {"127.0.0.1", 0}};
    const decltype(socket.local_endpoint()) endpoint = socket.local_endpoint();
    std::exception_ptr failure;
    std::thread thread;

    void start(pace::task<void> task)
    {
        io.spawn(std::move(task));
        thread = std::thread(
            [this]
            {
                try
                {
                    io.run();
                }
                catch (...)
                {
                    failure = std::current_exception();
                }
            }
        );
    }

    void join()
    {
        if (thread.joinable())
        {
            thread.join();
        }
        if (failure)
        {
            std::rethrow_exception(failure);
        }
    }

    void stop()
    {
        io.stop();
        join();
    }

    ~peer()
    {
        io.stop();
        if (thread.joinable())
        {
            thread.join();
        }
    }
};

pace::task<void> tcp_reply(pace::tcp::listener& listener, bool early_eof = false, bool stall = false)
{
    auto socket = co_await listener.accept();
    auto request = co_await pace::read_exact(socket, 3);
    require(request == std::string("a\0b", 3), "TCP request bytes changed");
    if (stall)
    {
        co_await pace::sleep_for(5s);
    }
    co_await pace::write(socket, request.substr(0, 1));
    if (!early_eof)
    {
        co_await pace::yield();
        co_await pace::write(socket, request.substr(1));
    }
    socket.close();
    listener.context().stop();
}

pace::task<void> udp_reply(pace::udp::socket& socket, std::string reply)
{
    auto request = co_await socket.receive();
    require(request.data == "hello", "UDP request changed");
    auto unrelated = pace::udp::open();
    co_await unrelated.send("wrong sender", request.sender);
    co_await pace::sleep_for(2ms);
    co_await socket.send(std::move(reply), request.sender);
    socket.context().stop();
}

pace::task<void> udp_collect(pace::udp::socket& socket, std::string& received)
{
    received = (co_await socket.receive()).data;
    socket.context().stop();
}
#ifdef PACE_HAS_HTTP1
static_assert(std::is_same_v<decltype(pace::http::get(std::string{})), pace::http::response>);
static_assert(std::is_same_v<decltype(pace::http::async_get(std::string{})), pace::task<pace::http::response>>);

pace::http::router routes()
{
    pace::http::router app;
    app.get("/", [] { return pace::http::text("ready"); });
    app.post("/echo", [](pace::http::request request) { return pace::http::text(std::move(request.body)); });
    app.get(
        "/slow",
        []() -> pace::task<pace::http::response>
        {
            co_await pace::sleep_for(5s);
            co_return pace::http::text("late");
        }
    );
    return app;
}
#endif
} // namespace

TEST_CASE("blocking and async TCP exchange return a fragmented binary reply")
{
    for (bool async : {false, true})
    {
        CAPTURE(async);
        peer<pace::tcp::listener> server;
        server.start(tcp_reply(server.socket));
        auto reply = async ? pace::run(pace::tcp::async_exchange(server.endpoint, std::string("a\0b", 3), 3))
                           : pace::tcp::exchange(server.endpoint, std::string("a\0b", 3), 3);
        require(reply == std::string("a\0b", 3), "TCP fragmented binary reply");
        require(!pace::io_context::current(), "blocking client leaked context");
        server.join();
    }
}

TEST_CASE("TCP exchange reports a premature EOF as connection reset")
{
    peer<pace::tcp::listener> server;
    server.start(tcp_reply(server.socket, true));
    bool failed = false;
    try
    {
        (void)pace::tcp::exchange(server.endpoint, std::string("a\0b", 3), 3);
    }
    catch (const std::system_error& e)
    {
        failed = e.code() == std::errc::connection_reset;
    }
    require(failed, "TCP premature EOF lost");
    server.join();
}

TEST_CASE("TCP exchange enforces its total deadline")
{
    peer<pace::tcp::listener> server;
    server.start(tcp_reply(server.socket, false, true));
    bool expired = false;
    try
    {
        (void)pace::tcp::exchange(server.endpoint, std::string("a\0b", 3), 3, {.timeout = 50ms});
    }
    catch (const std::system_error& e)
    {
        expired = e.code() == std::errc::timed_out;
    }
    require(expired, "TCP total deadline lost");
    server.stop();
}

TEST_CASE("TCP exchange rejects an oversized reply bound before connecting")
{
    CHECK_THROWS_AS((void)pace::tcp::exchange({"127.0.0.1", 1}, "", 10, {.max_reply_size = 9}), std::length_error);
}

TEST_CASE("UDP exchange filters other senders and truncates to capacity")
{
    for (auto capacity : {std::size_t{65536}, std::size_t{3}})
    {
        CAPTURE(capacity);
        peer<pace::udp::socket> server;
        const auto endpoint = server.endpoint;
        server.start(udp_reply(server.socket, "reply"));
        auto reply = pace::udp::exchange(endpoint, "hello", {.max_reply_size = capacity});
        require(
            reply.data == (capacity == 3 ? "rep" : "reply") && reply.truncated == (capacity == 3),
            "UDP filtering/truncation"
        );
        require(reply.sender.port == endpoint.port, "UDP source changed");
        server.join();
    }
}

TEST_CASE("async UDP exchange returns an empty reply")
{
    peer<pace::udp::socket> server;
    server.start(udp_reply(server.socket, ""));
    require(pace::run(pace::udp::async_exchange(server.endpoint, "hello")).data.empty(), "empty UDP reply");
    server.join();
}

TEST_CASE("UDP send delivers binary data and reports the byte count")
{
    peer<pace::udp::socket> server;
    std::string received;
    server.start(udp_collect(server.socket, received));
    require(pace::udp::send(server.endpoint, std::string("a\0b", 3)) == 3, "UDP send byte count");
    server.join();
    require(received == std::string("a\0b", 3), "UDP send binary data");
}

TEST_CASE("UDP exchange times out on a silent peer and rejects invalid timeouts")
{
    peer<pace::udp::socket> silent;
    bool expired = false;
    try
    {
        (void)pace::udp::exchange(silent.endpoint, "hello", {.timeout = 20ms});
    }
    catch (const std::system_error& e)
    {
        expired = e.code() == std::errc::timed_out;
    }
    require(expired, "UDP silent peer deadline");
    for (auto timeout : {0ms, -1ms})
    {
        CAPTURE(timeout.count());
        CHECK_THROWS_AS((void)pace::udp::send(silent.endpoint, "", {.timeout = timeout}), std::invalid_argument);
    }
}

TEST_CASE("blocking clients are rejected inside a reactor")
{
    pace::run(
        []() -> pace::task<void>
        {
            unsigned rejected = 0;
            try
            {
                (void)pace::tcp::exchange({"127.0.0.1", 1}, "", 0);
            }
            catch (const std::logic_error&)
            {
                ++rejected;
            }
            try
            {
                (void)pace::udp::exchange({"127.0.0.1", 1}, "");
            }
            catch (const std::logic_error&)
            {
                ++rejected;
            }
            try
            {
                (void)pace::udp::send({"127.0.0.1", 1}, "");
            }
            catch (const std::logic_error&)
            {
                ++rejected;
            }
            require(rejected == 3, "blocking sockets allowed inside reactor");
#ifdef PACE_HAS_HTTP1
            try
            {
                (void)pace::http::get("http://127.0.0.1/");
            }
            catch (const std::logic_error&)
            {
                ++rejected;
            }
            require(rejected == 4, "blocking HTTP allowed inside reactor");
#endif
            co_return;
        }
    );
}

#ifdef PACE_HAS_HTTP1
TEST_CASE("direct HTTP client methods, statuses, deadlines and URL validation")
{
    peer<pace::tcp::listener> server;
    server.start(pace::http::serve(server.socket, routes()));
    const auto url = "http://127.0.0.1:" + std::to_string(server.endpoint.port);
    require(pace::http::get(url).body == "ready", "direct GET");
#ifdef PACE_HAS_DNS
    require(
        pace::http::get("http://localhost:" + std::to_string(server.endpoint.port)).body == "ready",
        "direct HTTP hostname"
    );
#endif
    require(pace::http::post(url + "/echo", std::string("a\0b", 3)).body == std::string("a\0b", 3), "direct POST");
    pace::http::request head;
    head.method = "HEAD";
    auto response = pace::http::fetch(url, std::move(head));
    require(response.status == 200 && response.body.empty(), "direct HEAD fetch");
    require(pace::http::get(url + "/missing").status == 404, "HTTP status is not transport error");
    bool expired = false;
    try
    {
        (void)pace::http::get(url + "/slow", {.timeout = 20ms});
    }
    catch (const std::system_error& e)
    {
        expired = e.code() == std::errc::timed_out;
    }
    require(expired, "HTTP total deadline");
    require(pace::http::get(url).body == "ready", "later blocking call after timeout");
    CHECK_THROWS_AS((void)pace::http::get("invalid URL"), std::invalid_argument);
    server.stop();
}

#ifdef PACE_HAS_TLS
TEST_CASE("direct HTTPS client verifies the server certificate")
{
    const auto certs = pace::test::certificates();
    pace::tls::context security(pace::tls::role::server), trust;
    security.use_certificate_chain((certs / "localhost.pem").string(), (certs / "localhost-key.pem").string());
    security.set_alpn_protocols({"http/1.1"});
    trust.load_verify_file((certs / "ca.pem").string());
    peer<pace::tcp::listener> secure;
    secure.start(pace::http::serve(secure.socket, security, routes()));
    const auto secure_url = "https://127.0.0.1:" + std::to_string(secure.endpoint.port);
    require(pace::http::get(trust, secure_url).body == "ready", "direct verified HTTPS");
    CHECK_THROWS_AS((void)pace::http::get(secure_url), std::system_error);
    secure.stop();
}
#endif
#endif
TEST_CASE("async client options apply the selected context, cancellation and earliest deadline")
{
    pace::io_context other;
    auto before_run = pace::udp::open({.context = &other});
    require(&before_run.context() == &other, "explicit UDP context before run");
    pace::run(
        [&](pace::io_context& io) -> pace::task<void>
        {
            auto reserved = pace::tcp::listen({"127.0.0.1", 0});
            auto mismatch =
                co_await pace::tcp::connect(pace::as_result, reserved.local_endpoint(), {.context = &other});
            require(mismatch.error() == std::errc::operation_not_permitted, "foreign context accepted");
            pace::cancellation_source source;
            source.request_cancellation();
            bool attempted = false;
            pace::client_options options{.context = &io, .cancellation = source.token()};
            options.transport.pre_connect = [&](pace::native_socket_type) { attempted = true; };
            auto skipped = co_await pace::tcp::connect(pace::as_result, reserved.local_endpoint(), std::move(options));
            require(
                skipped.error() == std::errc::operation_canceled && !attempted,
                "pre-canceled connect attempted I/O"
            );
            auto silent = pace::udp::bind({"127.0.0.1", 0});
            auto timeout = co_await pace::as_result(
                pace::udp::async_exchange(silent.local_endpoint(), "q", {.context = &io, .timeout = 10ms})
            );
            require(timeout.error() == std::errc::timed_out, "async UDP timeout");
            auto expired = co_await pace::as_result(
                pace::udp::async_exchange(
                    silent.local_endpoint(),
                    "q",
                    {.timeout = 1s, .deadline = pace::deadline::after(0ms)}
                )
            );
            require(expired.error() == std::errc::timed_out, "absolute deadline ignored");
            auto parent = co_await pace::with_deadline(
                pace::deadline::after(5ms),
                pace::as_result(pace::udp::async_exchange(silent.local_endpoint(), "q", {.timeout = 1s}))
            );
            require(parent.error() == std::errc::timed_out, "client extended the parent deadline");
        }
    );
    CHECK_THROWS_AS((void)pace::udp::exchange({"127.0.0.1", 1}, "", {.context = &other}), std::invalid_argument);
}

TEST_CASE("TCP client options carry a move-only hook and enforce the reply bound before connecting")
{
    peer<pace::tcp::listener> server;
    server.start(tcp_reply(server.socket));
    unsigned calls = 0;
    auto settings = [&]
    {
        pace::client_options options{.timeout = 2s, .max_reply_size = 3};
        options.transport.pre_connect = [owned = std::make_unique<int>(1), &calls](pace::native_socket_type)
        { calls += *owned; };
        return options;
    };
    CHECK_THROWS_AS((void)pace::tcp::exchange(server.endpoint, "", 4, settings()), std::length_error);
    require(calls == 0, "TCP limit checked after opening connection");
    const auto request = std::string("a\0b", 3);
    require(pace::tcp::exchange(server.endpoint, request, 3, settings()) == request && calls == 1, "TCP options hook");
    server.join();
}

#ifdef PACE_HAS_TLS
TEST_CASE("TLS client options bound a verified binary exchange and a stalled handshake")
{
    const auto certs = pace::test::certificates();
    pace::tls::context security(pace::tls::role::server);
    security.use_certificate_chain((certs / "localhost.pem").string(), (certs / "localhost-key.pem").string());
    pace::tls::context trust(pace::tls::role::client, pace::tls::trust_source::application);
    trust.load_verify_file((certs / "ca.pem").string());
    const std::string payload("a\0b", 3);
    for (bool async : {false, true})
    {
        peer<pace::tcp::listener> server;
        auto echo = [&]() -> pace::task<void>
        {
            auto stream = co_await pace::tls::accept(co_await server.socket.accept(), security);
            auto request = co_await pace::read_exact(stream, payload.size());
            co_await pace::write(stream, std::move(request));
            co_await stream.shutdown();
            server.io.stop();
        };
        server.start(echo());
        bool connected = false;
        pace::client_options options{.timeout = 2s, .max_reply_size = 3, .tls = &trust};
        options.transport.pre_connect = [&](pace::native_socket_type) { connected = true; };
        auto reply =
            async ? pace::run(pace::tls::async_exchange(server.endpoint, "localhost", payload, 3, std::move(options)))
                  : pace::tls::exchange(server.endpoint, "localhost", payload, 3, std::move(options));
        require(reply == payload && connected, "TLS options were lost in the exchange");
        server.join();
    }
    peer<pace::tcp::listener> mismatch;
    auto accept_tls = [&]() -> pace::task<void>
    {
        auto incoming = co_await mismatch.socket.accept();
        (void)co_await pace::as_result(pace::tls::accept(std::move(incoming), security));
        mismatch.io.stop();
    };
    mismatch.start(accept_tls());
    auto denied = pace::run(
        pace::as_result(
            pace::tls::async_exchange(mismatch.endpoint, "wrong.example", "", 0, {.timeout = 2s, .tls = &trust})
        )
    );
    require(denied.error() == std::errc::protocol_error, "TLS client options bypassed peer name verification");
    mismatch.join();
    peer<pace::tcp::listener> silent;
    CHECK_THROWS_AS(
        (void)pace::tls::exchange(silent.endpoint, "localhost", "", 4, {.max_reply_size = 3, .tls = &trust}),
        std::length_error
    );
    auto expired =
        pace::run(pace::as_result(pace::tls::connect(silent.endpoint, "localhost", {.timeout = 20ms, .tls = &trust})));
    require(expired.error() == std::errc::timed_out, "TLS handshake outside the client timeout");
}
#endif
