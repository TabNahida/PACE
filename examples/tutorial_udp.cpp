#include <pace/pace.hpp>
#include <array>
#include <iostream>

pace::task<void> hello(pace::io_context& io) {
    pace::udp::socket receiver(io, {"127.0.0.1", 0});
    pace::udp::socket sender(io);
    co_await sender.send_to("Hello, UDP!", receiver.local_endpoint());
    std::array<char, 256> buffer{};
    auto packet = co_await receiver.receive_from(buffer);
    if (packet.truncated || std::string_view(buffer.data(), packet.size) != "Hello, UDP!")
        throw std::runtime_error("UDP message mismatch or truncation");
    std::cout << std::string_view(buffer.data(), packet.size) << '\n';
}
int main() {
    try { pace::run(hello); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
