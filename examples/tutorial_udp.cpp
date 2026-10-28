#include <pace/pace.hpp>
#include <iostream>

pace::task<void> hello() {
    auto receiver = pace::udp::bind({"127.0.0.1", 0});
    auto sender = pace::udp::open();
    co_await sender.send("Hello, UDP!", receiver.local_endpoint());
    auto packet = co_await receiver.receive();
    if (packet.truncated || packet.data != "Hello, UDP!")
        throw std::runtime_error("UDP message mismatch or truncation");
    std::cout << packet.data << '\n';
}
int main() {
    try { pace::run(hello); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
