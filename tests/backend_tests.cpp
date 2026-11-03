#include <pace/detail/poller.hpp>
#include <pace/udp.hpp>
#include <array>
#include <iostream>
#include <set>
#include <vector>
#include <cstdlib>
#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void notify(pace::native_socket_type sender, const pace::udp::socket& target) {
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(target.local_endpoint().port);
    require(::sendto(sender, "x", 1, 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 1, "send readiness marker");
}
void drain(pace::native_socket_type fd) { char byte; require(::recv(fd, &byte, 1, 0) == 1, "read readiness marker"); }
}
int main() {
    try {
        pace::io_context io;
        if (const auto* requested = std::getenv("PACE_IO_BACKEND"); requested && std::string_view(requested) != "automatic")
            require(pace::backend_name(io.backend()) == std::string_view(requested), "explicit backend was silently substituted");
        pace::detail::poller poller(false);
        std::cout << "backend=" << pace::backend_name(poller.backend()) << '\n';
        pace::udp::socket sender(io, {"127.0.0.1", 0});
        std::array<pace::detail::poll_event, 64> events{};
        // More subscriptions/cancellations than the 256-entry io_uring SQ.
        std::vector<pace::udp::socket> sockets;
        sockets.reserve(512);
        for (unsigned n = 0; n < 512; ++n) {
            sockets.emplace_back(io, pace::udp::endpoint{"127.0.0.1", 0});
            const auto fd = sockets.back().native_handle();
            poller.update(fd, true, false, n + 1);
            poller.remove(fd);
            poller.update(fd, true, false, n + 1001);
            notify(sender.native_handle(), sockets.back());
        }
        std::set<std::uint64_t> seen;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (seen.size() != sockets.size() && std::chrono::steady_clock::now() < end) {
            const auto count = poller.wait(events, 100);
            for (std::size_t n = 0; n < count; ++n) {
                require(events[n].token >= 1001 && events[n].token < 1513, "stale canceled subscription escaped");
                if (seen.insert(events[n].token).second) drain(sockets[events[n].token - 1001].native_handle());
            }
        }
        require(seen.size() == sockets.size(), "queue pressure lost a readiness notification");
        for (auto& socket : sockets) poller.forget(socket.native_handle());
        sockets.clear();
        // Native handles may be recycled; late packets must not target a new wait.
        for (unsigned n = 0; n < 128; ++n) {
            pace::udp::socket socket(io, {"127.0.0.1", 0});
            const auto token = 10000 + n;
            poller.update(socket.native_handle(), true, false, token);
            notify(sender.native_handle(), socket);
            bool ready = false;
            for (unsigned tries = 0; tries < 50 && !ready; ++tries) {
                const auto count = poller.wait(events, 100);
                for (std::size_t i = 0; i < count; ++i) {
                    require(events[i].token == token, "recycled socket received an obsolete token"); ready = true;
                }
            }
            require(ready, "recycled socket lost notification");
            drain(socket.native_handle()); poller.forget(socket.native_handle());
        }
        std::cout << "queue pressure, cancellation and descriptor reuse passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
