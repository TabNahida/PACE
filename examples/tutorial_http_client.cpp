#include <pace/http.hpp>

#include <iostream>
#include <stdexcept>

pace::task<pace::http::response> hello(pace::http::request request) {
    pace::http::response result;
    result.body = "Hello from " + request.target + "\n";
    result.chunked = true;
    result.trailers.push_back({"X-Example", "complete"});
    co_return result;
}

pace::task<void> use_http(pace::io_context& io, pace::tcp::endpoint endpoint) {
    // One expression handles connect, Host, response framing and bounded close.
    const auto response = co_await pace::http::get(io, endpoint, "/PACE");
    if (response.status != 200 || response.body != "Hello from /PACE\n" || response.trailers.size() != 1)
        throw std::runtime_error("HTTP client response mismatch");
    std::cout << response.body;
    io.stop();
}

int main() {
    try {
        pace::io_context io;
        pace::tcp::listener listener(io, {"127.0.0.1", 0});
        io.spawn(pace::http::serve(listener, hello));
        io.spawn(use_http(io, listener.local_endpoint()));
        io.run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
