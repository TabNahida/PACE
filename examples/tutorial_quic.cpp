#include <pace/quic.hpp>

#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
struct completion {
    pace::io_context& io;
    unsigned count = 0;
    void arrive() { if (++count == 2) io.stop(); }
};
pace::task<void> server(pace::quic::listener& listener, completion& done) {
    auto connection = co_await listener.accept();
    auto stream = co_await connection.accept_stream();
    std::array<char, 64> bytes{};
    std::string request;
    while (auto count = co_await stream.read_some(bytes)) request.append(bytes.data(), count);
    if (request != "hello") throw std::runtime_error("unexpected QUIC request");
    co_await stream.write_all("Hello over verified QUIC!\n");
    stream.finish_write();
    while (stream.pending_write_bytes()) co_await stream.wait_events();
    done.arrive();
}
pace::task<void> client(pace::quic::context config,
                       pace::udp::endpoint endpoint, completion& done) {
    auto connection = co_await pace::quic::connect(std::move(config), endpoint, "localhost");
    if (connection.selected_protocol() != "pace-demo") throw std::runtime_error("unexpected QUIC ALPN");
    auto stream = co_await connection.open_stream();
    co_await pace::write(stream, "hello");
    stream.finish_write(); // Half-close this stream; the response still arrives.
    auto response = co_await pace::read_exact(stream, std::string_view("Hello over verified QUIC!\n").size());
    std::array<char, 1> trailing{};
    if (response != "Hello over verified QUIC!\n" || co_await stream.read_some(trailing))
        throw std::runtime_error("QUIC response mismatch or missing FIN");
    std::cout << response;
    done.arrive();
}
pace::task<void> deadline(pace::io_context& io) {
    co_await io.sleep_for(std::chrono::seconds(5));
    throw std::runtime_error("QUIC tutorial timed out");
}
}
int main(int argc, char** argv) {
    try {
        if (argc > 2) throw std::invalid_argument("usage: tutorial_quic [test-certificate-directory]");
        const auto fixtures = argc == 2 ? std::filesystem::path(argv[1]) :
            std::filesystem::path(__FILE__).parent_path().parent_path() / "tests" / "certs";
        pace::quic::context server_config(pace::quic::role::server), client_config;
        // Public test credentials are only for this local demonstration.
        server_config.use_certificate_chain((fixtures / "localhost.pem").string(), (fixtures / "localhost-key.pem").string());
        client_config.load_verify_file((fixtures / "ca.pem").string());
        server_config.set_alpn_protocols({"pace-demo"});
        client_config.set_alpn_protocols({"pace-demo"});
        pace::io_context io;
        auto listener = pace::quic::listen(io, {"127.0.0.1", 0}, server_config);
        completion done{io};
        io.spawn(server(listener, done));
        io.spawn(client(client_config, listener.local_endpoint(), done));
        io.spawn(deadline(io));
        io.run();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
