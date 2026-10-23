#include <pace/http2.hpp>
#include <pace/http3.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Cooperative application concurrency on one reactor. The artificial timer
// models independent asynchronous work; it is not CPU parallelism or a ranking
// against another HTTP implementation. Every response is checked before timing
// is reported. TCP_NODELAY is enabled on both TCP/TLS peers.
namespace {
using clock_type = std::chrono::steady_clock;
struct options {
    std::string protocol = "h2";
    unsigned handlers = 8, streams = 16, delay_ms = 10, batches = 5, body_bytes = 1024;
    std::filesystem::path certs = std::filesystem::path(__FILE__).parent_path().parent_path() / "tests/certs";
};
unsigned number(const std::string& text, unsigned limit) {
    std::size_t used = 0; const auto value = std::stoul(text, &used);
    if (used != text.size() || value > limit) throw std::invalid_argument("invalid number");
    return static_cast<unsigned>(value);
}
options parse(int argc, char** argv) {
    options result;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc) throw std::invalid_argument("options require values");
        const std::string key = argv[i], value = argv[i + 1];
        if (key == "--protocol") result.protocol = value;
        else if (key == "--handlers") result.handlers = number(value, 10000);
        else if (key == "--streams") result.streams = number(value, 10000);
        else if (key == "--delay-ms") result.delay_ms = number(value, 1000);
        else if (key == "--batches") result.batches = number(value, 1000);
        else if (key == "--body-bytes") result.body_bytes = number(value, 1024 * 1024);
        else if (key == "--certs") result.certs = value;
        else throw std::invalid_argument("unknown option: " + key);
    }
    if (!result.handlers || !result.streams || !result.batches ||
        (result.protocol != "h2" && result.protocol != "h2tls" && result.protocol != "h3"))
        throw std::invalid_argument("invalid configuration");
    return result;
}
struct run_state {
    pace::io_context io;
    const options& config;
    unsigned active = 0, peak = 0, complete = 0;
    explicit run_state(const options& value) : config(value) {}
    pace::task<pace::http::response> handle(pace::http::request request) {
        peak = std::max(peak, ++active);
        if (config.delay_ms) co_await io.sleep_for(std::chrono::milliseconds(config.delay_ms));
        --active;
        const auto index = number(request.target.substr(1), config.streams);
        co_return pace::http::response{.body = std::string(config.body_bytes, static_cast<char>('a' + index % 26))};
    }
    std::vector<pace::http::request> requests() const {
        std::vector<pace::http::request> batch(config.streams);
        for (unsigned i = 0; i < config.streams; ++i) { batch[i].method = "GET"; batch[i].target = "/" + std::to_string(i); }
        return batch;
    }
    void record(const std::vector<pace::http::response>& replies, clock_type::time_point begin, unsigned trial) {
        const auto elapsed = std::chrono::duration<double, std::milli>(clock_type::now() - begin).count();
        if (replies.size() != config.streams || active || peak > config.handlers) throw std::runtime_error("invalid concurrency");
        for (unsigned i = 0; i < replies.size(); ++i)
            if (replies[i].status != 200 || replies[i].body != std::string(config.body_bytes, static_cast<char>('a' + i % 26)))
                throw std::runtime_error("invalid response");
        std::cout << config.protocol << ',' << config.handlers << ',' << config.streams << ',' << config.delay_ms << ','
            << config.body_bytes << ',' << trial << ',' << elapsed << ',' << peak << ",true\n";
        peak = 0; ++complete;
    }
};
#ifdef PACE_HAS_HTTP2
pace::http2::limits h2_limits(const options& config) {
    pace::http2::limits bounds; bounds.max_concurrent_streams = config.streams;
    bounds.max_requests_per_connection = config.streams; bounds.max_concurrent_handlers = config.handlers; return bounds;
}
template<class Stream> pace::task<void> h2_serve(Stream stream, run_state& state) {
    try {
        co_await pace::http2::serve_connection(std::move(stream), [&](pace::http::request request) { return state.handle(std::move(request)); }, h2_limits(state.config));
    } catch (const std::system_error& error) { if (error.code() != std::errc::operation_canceled) throw; }
}
pace::task<void> tcp_accept(pace::tcp::listener& listener, run_state& state) {
    try { for (;;) { auto stream = co_await listener.accept(); stream.set_no_delay(); state.io.spawn(h2_serve(std::move(stream), state)); } }
    catch (const std::system_error& error) { if (error.code() != std::errc::operation_canceled) throw; }
}
pace::task<void> tcp_client(pace::tcp::endpoint endpoint, run_state& state) {
    for (unsigned trial = 0; trial < state.config.batches; ++trial) {
        pace::tcp::stream stream(state.io); stream.set_no_delay(); co_await stream.connect(endpoint);
        auto requests = state.requests(); const auto begin = clock_type::now();
        const auto replies = co_await pace::http2::exchange(std::move(stream), "localhost", std::move(requests), h2_limits(state.config));
        state.record(replies, begin, trial);
    }
    state.io.stop();
}
#ifdef PACE_HAS_TLS
pace::task<void> tls_serve(pace::tls::stream stream, run_state& state) {
    co_await stream.handshake(); co_await h2_serve(std::move(stream), state);
}
pace::task<void> tls_accept(pace::tcp::listener& listener, const pace::tls::context& config, run_state& state) {
    try { for (;;) { auto stream = co_await listener.accept(); stream.set_no_delay(); state.io.spawn(tls_serve(pace::tls::stream(std::move(stream), config), state)); } }
    catch (const std::system_error& error) { if (error.code() != std::errc::operation_canceled) throw; }
}
pace::task<void> tls_client(pace::tcp::endpoint endpoint, const pace::tls::context& config, run_state& state) {
    for (unsigned trial = 0; trial < state.config.batches; ++trial) {
        pace::tls::stream stream(state.io, config); stream.set_no_delay(); co_await stream.connect(endpoint, "localhost");
        auto requests = state.requests(); const auto begin = clock_type::now();
        const auto replies = co_await pace::http2::exchange(std::move(stream), "localhost", std::move(requests), h2_limits(state.config));
        state.record(replies, begin, trial);
    }
    state.io.stop();
}
#endif
#endif
#ifdef PACE_HAS_HTTP3
pace::task<void> h3_client(pace::udp::endpoint endpoint, const pace::quic::context& config, run_state& state) {
    pace::http3::limits bounds; bounds.max_concurrent_streams = state.config.streams;
    bounds.max_requests_per_connection = state.config.streams; bounds.max_concurrent_handlers = state.config.handlers;
    for (unsigned trial = 0; trial < state.config.batches; ++trial) {
        pace::quic::connection connection(state.io, config); co_await connection.connect(endpoint, "localhost");
        auto requests = state.requests(); const auto begin = clock_type::now();
        const auto replies = co_await pace::http3::exchange(std::move(connection), "localhost", std::move(requests), bounds);
        state.record(replies, begin, trial);
    }
    state.io.stop();
}
#endif
void run(const options& config) {
    run_state state(config);
    std::cout << "protocol,handlers,streams,handler_delay_ms,body_bytes,trial,batch_ms,peak_handlers,validated\n" << std::fixed << std::setprecision(6);
#ifdef PACE_HAS_HTTP2
    if (config.protocol == "h2") {
        pace::tcp::listener listener(state.io, {"127.0.0.1", 0});
        state.io.spawn(tcp_accept(listener, state)); state.io.spawn(tcp_client(listener.local_endpoint(), state)); state.io.run();
    }
#ifdef PACE_HAS_TLS
    else if (config.protocol == "h2tls") {
        pace::tls::context server(pace::tls::role::server), client(pace::tls::role::client, pace::tls::trust_source::application);
        server.use_certificate_chain((config.certs / "localhost.pem").string(), (config.certs / "localhost-key.pem").string());
        client.load_verify_file((config.certs / "ca.pem").string()); server.set_alpn_protocols({"h2"}); client.set_alpn_protocols({"h2"});
        pace::tcp::listener listener(state.io, {"127.0.0.1", 0});
        state.io.spawn(tls_accept(listener, server, state)); state.io.spawn(tls_client(listener.local_endpoint(), client, state)); state.io.run();
    }
#endif
#endif
#ifdef PACE_HAS_HTTP3
    if (config.protocol == "h3") {
        pace::quic::context server(pace::quic::role::server), client;
        server.use_certificate_chain((config.certs / "localhost.pem").string(), (config.certs / "localhost-key.pem").string());
        client.load_verify_file((config.certs / "ca.pem").string()); server.set_alpn_protocols({"h3"}); client.set_alpn_protocols({"h3"});
        pace::quic::listener listener(state.io, {"127.0.0.1", 0}, server);
        pace::http3::limits bounds; bounds.max_concurrent_streams = config.streams;
        bounds.max_requests_per_connection = config.streams; bounds.max_concurrent_handlers = config.handlers;
        state.io.spawn(pace::http3::serve(listener, [&](pace::http::request request) { return state.handle(std::move(request)); }, bounds));
        state.io.spawn(h3_client(listener.local_endpoint(), client, state)); state.io.run();
    }
#endif
    if (state.complete != config.batches) throw std::runtime_error("protocol not enabled or incomplete run");
}
} // namespace
int main(int argc, char** argv) {
    try { run(parse(argc, argv)); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
