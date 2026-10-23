#include <pace/dns.hpp>
#include <iostream>

pace::task<void> lookup(pace::io_context& io, std::string hostname) {
    pace::dns::resolver resolver(io);
    const auto result = co_await resolver.resolve(std::move(hostname));
    for (const auto& address : result.addresses)
        std::cout << (address.type == pace::dns::family::ipv4 ? "IPv4 " : "IPv6 ") << address.text << '\n';
    io.stop();
}
int main(int argc, char** argv) {
    try {
        if (argc > 2) throw std::invalid_argument("usage: tutorial_dns [hostname]");
        pace::io_context io;
        // Numeric default is deterministic and requires no external network.
        io.spawn(lookup(io, argc == 2 ? argv[1] : "127.0.0.1"));
        io.run();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
