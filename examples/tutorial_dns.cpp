#include <pace/dns.hpp>
#include <pace/run.hpp>
#include <iostream>

pace::task<void> lookup(pace::io_context& io, std::string hostname) {
    const auto result = co_await pace::dns::resolve(io, std::move(hostname));
    for (const auto& address : result.addresses)
        std::cout << (address.type == pace::dns::family::ipv4 ? "IPv4 " : "IPv6 ") << address.text << '\n';
}
int main(int argc, char** argv) {
    try {
        if (argc > 2) throw std::invalid_argument("usage: tutorial_dns [hostname]");
        // Numeric default is deterministic and requires no external network.
        pace::run([hostname = std::string(argc == 2 ? argv[1] : "127.0.0.1")](pace::io_context& io) {
            return lookup(io, hostname);
        });
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
