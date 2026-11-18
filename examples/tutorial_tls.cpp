#include <pace/tls.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
struct completion {
    pace::io_context& io;
    unsigned peers = 0;
    void finish() { if (++peers == 2) io.stop(); }
};

pace::task<void> server(pace::tcp::listener& listener, const pace::tls::context& config,
                        completion& done) {
    auto connection = co_await pace::tls::accept(co_await listener.accept(), config);
    co_await pace::write(connection, "Hello over verified TLS!\n");
    co_await connection.shutdown();
    done.finish();
}

pace::task<void> client(pace::io_context& io, const pace::tls::context& config,
                        pace::tcp::endpoint endpoint, completion& done) {
    auto connection = co_await pace::tls::connect(io, config, std::move(endpoint), "localhost");
    if (connection.selected_protocol() != "pace-demo")
        throw std::runtime_error("unexpected negotiated protocol");
    auto received = co_await pace::read_exact(connection, std::string_view("Hello over verified TLS!\n").size());
    std::array<char, 1> end{};
    if (co_await connection.read_some(end) != 0) throw std::runtime_error("extra TLS response bytes");
    if (received != "Hello over verified TLS!\n") throw std::runtime_error("TLS payload mismatch");
    // read_some returned authenticated close_notify, not bare TCP EOF.
    co_await connection.shutdown();
    std::cout << received;
    done.finish();
}

pace::task<void> deadline(pace::io_context& io) {
    co_await io.sleep_for(std::chrono::seconds(5));
    throw std::runtime_error("TLS tutorial did not complete within five seconds");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 2) throw std::invalid_argument("usage: tutorial_tls [test-certificate-directory]");
        const auto fixtures = argc == 2 ? std::filesystem::path(argv[1]) :
            std::filesystem::path(__FILE__).parent_path().parent_path() / "tests" / "certs";
        pace::tls::context server_config(pace::tls::role::server), client_config;
        // These checked-in credentials are exclusively for this loopback tutorial.
        // Real servers must provide their own chain/key and real client trust.
        server_config.use_certificate_chain((fixtures / "localhost.pem").string(),
                                             (fixtures / "localhost-key.pem").string());
        client_config.load_verify_file((fixtures / "ca.pem").string());
        server_config.set_alpn_protocols({"pace-demo"});
        client_config.set_alpn_protocols({"pace-demo"});
        pace::io_context io;
        pace::tcp::listener listener(io, {"127.0.0.1", 0});
        completion done{io};
        io.spawn(server(listener, server_config, done));
        io.spawn(client(io, client_config, listener.local_endpoint(), done));
        io.spawn(deadline(io));
        io.run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
