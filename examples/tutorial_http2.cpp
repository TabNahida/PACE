#include <pace/pace.hpp>
#include <iostream>

pace::task<pace::http::response> hello(pace::http::request request) {
    co_return pace::http::response{.body="Hello, " + request.target + "!"};
}
pace::task<void> client(pace::io_context& io, pace::tcp::endpoint endpoint) {
    auto first = co_await pace::http2::get(co_await pace::tcp::connect(endpoint), "localhost", "/hello");
    if (first.status != 200 || first.body != "Hello, /hello!") throw std::runtime_error("HTTP2 GET mismatch");
    auto stream = co_await pace::tcp::connect(endpoint);
    std::vector<pace::http::request> requests(2);
    requests[0].method="GET"; requests[0].target="/one";
    requests[1].method="GET"; requests[1].target="/two";
    auto responses=co_await pace::http2::exchange(std::move(stream),"localhost",std::move(requests));
    for(const auto& response : responses) std::cout<<response.body<<'\n';
    io.stop();
}
int main() {
    try {
        pace::io_context io;
        pace::tcp::listener listener(io,{"127.0.0.1",0});
        io.spawn(pace::http2::serve(listener,hello));
        io.spawn(client(io,listener.local_endpoint()));
        io.run();
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
