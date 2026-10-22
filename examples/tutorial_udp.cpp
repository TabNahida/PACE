#include <pace/pace.hpp>
#include <array>
#include <iostream>

pace::task<void> hello(pace::io_context& io) {
    pace::udp::socket receiver(io, {"127.0.0.1", 0});
    pace::udp::socket sender(io);
    co_await sender.send_to("Hello, UDP!", receiver.local_endpoint());
    std::array<char, 256> buffer{};
    auto packet = co_await receiver.receive_from(buffer);
    std::cout << std::string_view(buffer.data(), packet.size) << '\n';
    io.stop();
}
int main() {
    try { pace::io_context io; io.spawn(hello(io)); io.run(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
