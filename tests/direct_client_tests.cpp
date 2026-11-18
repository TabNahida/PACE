#include <pace/pace.hpp>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#include <type_traits>

namespace {
using namespace std::chrono_literals;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct watchdog {
    std::mutex mutex;
    std::condition_variable wake;
    bool finished = false;
    std::thread worker{[this] {
        std::unique_lock lock(mutex);
        if (!wake.wait_for(lock, 45s, [&] { return finished; })) std::abort();
    }};
    ~watchdog() { { std::lock_guard lock(mutex); finished = true; } wake.notify_one(); worker.join(); }
};
template<class Socket>
struct peer {
    pace::io_context io;
    Socket socket{io, {"127.0.0.1", 0}};
    const decltype(socket.local_endpoint()) endpoint = socket.local_endpoint();
    std::exception_ptr failure;
    std::thread thread;
    void start(pace::task<void> task) {
        io.spawn(std::move(task));
        thread = std::thread([this] { try { io.run(); } catch (...) { failure = std::current_exception(); } });
    }
    void join() { if (thread.joinable()) thread.join(); if (failure) std::rethrow_exception(failure); }
    void stop() { io.stop(); join(); }
    ~peer() { io.stop(); if (thread.joinable()) thread.join(); }
};
pace::task<void> tcp_reply(pace::tcp::listener& listener, bool early_eof = false, bool stall = false) {
    auto socket = co_await listener.accept();
    auto request = co_await pace::read_exact(socket, 3);
    require(request == std::string("a\0b", 3), "TCP request bytes changed");
    if (stall) co_await pace::sleep_for(5s);
    co_await pace::write(socket, request.substr(0, 1));
    if (!early_eof) { co_await pace::yield(); co_await pace::write(socket, request.substr(1)); }
    socket.close();
    listener.context().stop();
}
void tcp_checks() {
    for (bool async : {false, true}) {
        peer<pace::tcp::listener> server;
        server.start(tcp_reply(server.socket));
        auto reply = async ? pace::run(pace::tcp::async_exchange(server.endpoint, std::string("a\0b", 3), 3))
                           : pace::tcp::exchange(server.endpoint, std::string("a\0b", 3), 3);
        require(reply == std::string("a\0b", 3), "TCP fragmented binary reply");
        require(!pace::io_context::current(), "blocking client leaked context");
        server.join();
    }
    {
        peer<pace::tcp::listener> server; server.start(tcp_reply(server.socket, true));
        bool failed = false;
        try { (void)pace::tcp::exchange(server.endpoint, std::string("a\0b", 3), 3); }
        catch (const std::system_error& e) { failed = e.code() == std::errc::connection_reset; }
        require(failed, "TCP premature EOF lost"); server.join();
    }
    {
        peer<pace::tcp::listener> server; server.start(tcp_reply(server.socket, false, true));
        bool expired = false;
        try { (void)pace::tcp::exchange(server.endpoint, std::string("a\0b", 3), 3, {.timeout=50ms}); }
        catch (const std::system_error& e) { expired = e.code() == std::errc::timed_out; }
        require(expired, "TCP total deadline lost"); server.stop();
    }
    bool bounded = false;
    try { (void)pace::tcp::exchange({"127.0.0.1", 1}, "", 10, {}, 9); }
    catch (const std::length_error&) { bounded = true; }
    require(bounded, "TCP bound must reject before connect");
}
pace::task<void> udp_reply(pace::udp::socket& socket, std::string reply) {
    auto request = co_await socket.receive();
    require(request.data == "hello", "UDP request changed");
    auto unrelated = pace::udp::open();
    co_await unrelated.send("wrong sender", request.sender);
    co_await pace::sleep_for(2ms);
    co_await socket.send(std::move(reply), request.sender);
    socket.context().stop();
}
pace::task<void> udp_collect(pace::udp::socket& socket, std::string& received) {
    received = (co_await socket.receive()).data;
    socket.context().stop();
}
void udp_checks() {
    for (auto capacity : {std::size_t{65536}, std::size_t{3}}) {
        peer<pace::udp::socket> server;
        const auto endpoint = server.endpoint;
        server.start(udp_reply(server.socket, "reply"));
        auto reply = pace::udp::exchange(endpoint, "hello", {}, capacity);
        require(reply.data == (capacity == 3 ? "rep" : "reply") && reply.truncated == (capacity == 3), "UDP filtering/truncation");
        require(reply.sender.port == endpoint.port, "UDP source changed"); server.join();
    }
    {
        peer<pace::udp::socket> server; server.start(udp_reply(server.socket, ""));
        require(pace::run(pace::udp::async_exchange(server.endpoint, "hello")).data.empty(), "empty UDP reply"); server.join();
    }
    {
        peer<pace::udp::socket> server; std::string received;
        server.start(udp_collect(server.socket, received));
        require(pace::udp::send(server.endpoint, std::string("a\0b", 3)) == 3, "UDP send byte count");
        server.join(); require(received == std::string("a\0b", 3), "UDP send binary data");
    }
    peer<pace::udp::socket> silent;
    bool expired = false;
    try { (void)pace::udp::exchange(silent.endpoint, "hello", {.timeout=20ms}); }
    catch (const std::system_error& e) { expired = e.code() == std::errc::timed_out; }
    require(expired, "UDP silent peer deadline");
    for (auto timeout : {0ms, -1ms}) {
        bool rejected = false;
        try { (void)pace::udp::send(silent.endpoint, "", {.timeout=timeout}); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "invalid blocking timeout");
    }
}
void nested_checks() {
    pace::run([]() -> pace::task<void> {
        unsigned rejected = 0;
        try { (void)pace::tcp::exchange({"127.0.0.1", 1}, "", 0); } catch (const std::logic_error&) { ++rejected; }
        try { (void)pace::udp::exchange({"127.0.0.1", 1}, ""); } catch (const std::logic_error&) { ++rejected; }
        try { (void)pace::udp::send({"127.0.0.1", 1}, ""); } catch (const std::logic_error&) { ++rejected; }
        require(rejected == 3, "blocking sockets allowed inside reactor");
#ifdef PACE_HAS_HTTP1
        try { (void)pace::http::get("http://127.0.0.1/"); } catch (const std::logic_error&) { ++rejected; }
        require(rejected == 4, "blocking HTTP allowed inside reactor");
#endif
        co_return;
    });
}
#ifdef PACE_HAS_HTTP1
static_assert(std::is_same_v<decltype(pace::http::get(std::string{})), pace::http::response>);
static_assert(std::is_same_v<decltype(pace::http::async_get(std::string{})), pace::task<pace::http::response>>);
pace::http::router routes() {
    pace::http::router app;
    app.get("/", [] { return pace::http::text("ready"); });
    app.post("/echo", [](pace::http::request request) { return pace::http::text(std::move(request.body)); });
    app.get("/slow", []() -> pace::task<pace::http::response> {
        co_await pace::sleep_for(5s); co_return pace::http::text("late");
    });
    return app;
}
void http_checks() {
    peer<pace::tcp::listener> server; server.start(pace::http::serve(server.socket, routes()));
    const auto url = "http://127.0.0.1:" + std::to_string(server.endpoint.port);
    require(pace::http::get(url).body == "ready", "direct GET");
#ifdef PACE_HAS_DNS
    require(pace::http::get("http://localhost:" + std::to_string(server.endpoint.port)).body == "ready", "direct HTTP hostname");
#endif
    require(pace::http::post(url + "/echo", std::string("a\0b", 3)).body == std::string("a\0b", 3), "direct POST");
    pace::http::request head; head.method = "HEAD";
    auto response = pace::http::fetch(url, std::move(head));
    require(response.status == 200 && response.body.empty(), "direct HEAD fetch");
    require(pace::http::get(url + "/missing").status == 404, "HTTP status is not transport error");
    bool expired = false;
    try { (void)pace::http::get(url + "/slow", {.timeout=20ms}); }
    catch (const std::system_error& e) { expired = e.code() == std::errc::timed_out; }
    require(expired, "HTTP total deadline");
    require(pace::http::get(url).body == "ready", "later blocking call after timeout");
    bool malformed = false;
    try { (void)pace::http::get("invalid URL"); } catch (const std::invalid_argument&) { malformed = true; }
    require(malformed, "HTTP URL error lost");
    server.stop();
#ifdef PACE_HAS_TLS
    const auto certs = std::filesystem::path(__FILE__).parent_path()/"certs";
    pace::tls::context security(pace::tls::role::server), trust;
    security.use_certificate_chain((certs/"localhost.pem").string(), (certs/"localhost-key.pem").string());
    security.set_alpn_protocols({"http/1.1"});
    trust.load_verify_file((certs/"ca.pem").string());
    peer<pace::tcp::listener> secure; secure.start(pace::http::serve(secure.socket, security, routes()));
    const auto secure_url = "https://127.0.0.1:" + std::to_string(secure.endpoint.port);
    require(pace::http::get(trust, secure_url).body == "ready", "direct verified HTTPS");
    bool untrusted = false;
    try { (void)pace::http::get(secure_url); } catch (const std::system_error&) { untrusted = true; }
    require(untrusted, "direct HTTPS disabled verification");
    secure.stop();
#endif
}
#endif
} // namespace
int main() {
    try {
        watchdog timeout;
        tcp_checks(); udp_checks(); nested_checks();
#ifdef PACE_HAS_HTTP1
        http_checks();
#endif
        std::cout << "Direct clients, async alternatives, ownership, framing and total deadlines passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
