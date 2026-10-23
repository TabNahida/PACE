#include <pace/http3.hpp>
#include <filesystem>
#include <iostream>

pace::task<pace::http::response> hello(pace::http::request request) {
    co_return pace::http::response{.body = "Hello over HTTP/3, " + request.target + "!"};
}
pace::task<void> client(pace::io_context& io, pace::udp::endpoint endpoint, const pace::quic::context& config) {
    pace::quic::connection connection(io, config);
    co_await connection.connect(endpoint, "localhost");
    std::vector<pace::http::request> requests(2);
    requests[0].method = "GET"; requests[0].target = "/one";
    requests[1].method = "GET"; requests[1].target = "/two";
    auto responses = co_await pace::http3::exchange(std::move(connection), "localhost", std::move(requests));
    for (const auto& response : responses) std::cout << response.body << '\n';
    io.stop();
}
int main(int argc, char** argv) {
    try {
        // Public localhost fixtures are only for this self-contained tutorial.
        const auto certs = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(__FILE__).parent_path().parent_path()/"tests/certs";
        pace::io_context io;
        pace::quic::context server_config(pace::quic::role::server), client_config;
        server_config.use_certificate_chain((certs/"localhost.pem").string(), (certs/"localhost-key.pem").string());
        client_config.load_verify_file((certs/"ca.pem").string());
        server_config.set_alpn_protocols({"h3"}); client_config.set_alpn_protocols({"h3"});
        pace::quic::listener listener(io, {"127.0.0.1", 0}, server_config);
        io.spawn(pace::http3::serve(listener, hello));
        io.spawn(client(io, listener.local_endpoint(), client_config));
        io.run();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
