#include <pace/udp.hpp>
#include <pace/detail/socket_address.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
using namespace std::chrono_literals;
void require(bool value, const char* what) { if (!value) throw std::runtime_error(what); }
pace::task<void> owned_packets(pace::io_context& io) {
    auto receiver = pace::udp::bind({"127.0.0.1", 0});
    auto sender = pace::udp::open(io);
    auto pending = sender.send(std::string("a\0b", 3), receiver.local_endpoint());
    co_await io.yield();
    require((co_await std::move(pending)) == 3, "owned UDP lazy send");
    auto packet = co_await receiver.receive(3);
    require(packet.data == std::string("a\0b", 3) && !packet.truncated &&
            packet.sender.port == sender.local_endpoint().port, "owned UDP packet/source");
    co_await receiver.send("reply", packet.sender);
    require((co_await sender.receive()).data == "reply", "owned UDP reply");
    co_await sender.send("", receiver.local_endpoint());
    auto empty = co_await receiver.receive();
    require(empty.data.empty() && !empty.truncated, "empty UDP packet is not EOF");
    co_await sender.send("123456", receiver.local_endpoint());
    auto cut = co_await receiver.receive(3);
    require(cut.data == "123" && cut.truncated, "owned UDP truncation flag");
    for (auto capacity : {std::size_t{0}, std::size_t{65537}}) {
        bool rejected = false;
        try { (void)co_await receiver.receive(capacity); }
        catch (const std::logic_error&) { rejected = true; }
        require(rejected, "owned receive invalid allocation bound");
    }
    auto timeout = io.call_after(1ms, [&] { receiver.close(); });
    bool canceled = false;
    try { (void)co_await receiver.receive(); }
    catch (const std::system_error& error) { canceled = error.code() == std::errc::operation_canceled; }
    require(canceled, "owned receive lost cancellation");
    io.stop();
}

pace::task<void> roundtrip(pace::io_context& io) {
    pace::udp::socket receiver(io, {"127.0.0.1", 0}), sender(io, {"127.0.0.1", 0});
    auto destination = receiver.local_endpoint();
    const auto source = sender.local_endpoint();
    sender.set_broadcast(); sender.set_broadcast(false);
    std::string data("a\0b\xff", 4);
    require(co_await sender.send_to(data, destination) == data.size(), "send length");
    std::array<char, 16> buffer{};
    auto packet = co_await receiver.receive_from(buffer);
    require(packet.size == data.size() && !packet.truncated &&
            std::string_view(buffer.data(), packet.size) == data, "binary datagram mismatch");
    require(packet.sender.address == source.address && packet.sender.port == source.port, "sender endpoint");
    co_await receiver.send_to("reply", packet.sender);
    require((co_await sender.receive_from(buffer)).size == 5, "reply");
    co_await sender.send_to(std::string_view{}, destination);
    require((co_await receiver.receive_from(buffer)).size == 0, "zero datagram must not mean EOF");
    co_await sender.send_to("123456789", destination);
    auto cut = co_await receiver.receive_from(std::span<char>(buffer.data(), 3));
    require(cut.size == 3 && cut.truncated && std::string_view(buffer.data(), 3) == "123", "truncated datagram");
    co_await sender.send_to("next", destination);
    packet = co_await receiver.receive_from(buffer);
    require(packet.size == 4 && !packet.truncated && std::string_view(buffer.data(), 4) == "next", "datagram boundary");
    std::string oversized(65508, 'x');
    bool rejected = false;
    try { co_await sender.send_to(oversized, destination); }
    catch (const std::system_error& e) { rejected = e.code() == std::errc::message_size; }
    require(rejected, "oversize send");
    rejected = false;
    try { co_await receiver.receive_from(std::span<char>{}); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "empty receive buffer");
    pace::udp::socket moved(std::move(sender));
    require(moved.is_open() && !sender.is_open(), "move transfers ownership");
    moved.close(); moved.close();
    rejected = false;
    try { co_await moved.send_to("x", destination); }
    catch (const std::system_error& e) { rejected = e.code() == std::errc::bad_file_descriptor; }
    require(rejected, "closed socket");
    io.stop();
}

pace::task<void> ipv6_roundtrip(pace::io_context& io) {
    using pace::address_family;
    pace::udp::socket receiver(io, {"::1", 0}), sender(io);
    const auto destination = receiver.local_endpoint();
    require(destination.family == address_family::ipv6 && destination.address == "::1" &&
            destination.port != 0 && destination.scope_id == 0, "IPv6 bound UDP endpoint");
    const std::string payload("IPv6\0UDP", 8);
    co_await sender.send_to(payload, destination); // First send chooses IPv6.
    const auto source = sender.local_endpoint();
    require(source.family == address_family::ipv6 && source.port != 0, "IPv6 UDP automatic bind");
    std::array<char, 32> bytes{};
    auto packet = co_await receiver.receive_from(bytes);
    require(packet.sender.family == address_family::ipv6 && packet.sender.address == "::1" &&
            packet.sender.port == source.port && packet.sender.scope_id == 0 &&
            packet.size == payload.size() && std::string_view(bytes.data(), packet.size) == payload,
            "IPv6 UDP payload and source inspection");
    co_await receiver.send_to("reply", packet.sender);
    require((co_await sender.receive_from(bytes)).size == 5, "IPv6 UDP reply");
    co_await sender.send_to(std::string_view{}, destination);
    require((co_await receiver.receive_from(bytes)).size == 0, "IPv6 empty datagram");
    co_await sender.send_to("truncated", destination);
    packet = co_await receiver.receive_from(std::span<char>(bytes).first(3));
    require(packet.truncated && packet.size == 3 && packet.sender.family == address_family::ipv6,
            "IPv6 datagram truncation");
    bool rejected = false;
    try { co_await sender.send_to("wrong family", {"127.0.0.1", destination.port}); }
    catch (const std::system_error& error) { rejected = error.code() == std::errc::address_family_not_supported; }
    require(rejected, "selected UDP socket family must not change");
    rejected = false;
    try { receiver.set_broadcast(); }
    catch (const std::system_error& error) { rejected = error.code() == std::errc::operation_not_supported; }
    require(rejected, "IPv6 UDP broadcast must be rejected");
    pace::udp::socket moved(std::move(sender));
    pace::udp::socket assigned(io);
    assigned = std::move(moved);
    require(assigned.local_endpoint().family == address_family::ipv6 &&
            assigned.local_endpoint().port == source.port, "IPv6 UDP move ownership");
    co_await assigned.send_to("moved", destination);
    require((co_await receiver.receive_from(bytes)).size == 5, "IPv6 UDP send after move");
    pace::udp::socket fixed(io, address_family::ipv6);
    const auto handle = fixed.native_handle();
    fixed.bind({"::1", 0});
    require(fixed.native_handle() == handle, "explicit UDP family changed native handle");
    co_await fixed.send_to("fixed", destination);
    require((co_await receiver.receive_from(bytes)).size == 5, "explicit IPv6 UDP socket");
    io.stop();
}

pace::task<void> ipv6_wildcards(pace::io_context& io) {
    using pace::address_family;
    pace::udp::socket only_v6(io, {"::", 0});
    const auto local = only_v6.local_endpoint();
    require(local.address == "::" && local.family == address_family::ipv6, "IPv6 UDP wildcard");
    int only = 0;
    pace::detail::socket_address_length length = sizeof(only);
    require(::getsockopt(only_v6.native_handle(), IPPROTO_IPV6, IPV6_V6ONLY,
                        reinterpret_cast<char*>(&only), &length) == 0 && only == 1,
            "UDP IPv6-only default");
    pace::udp::socket v4(io, {"0.0.0.0", local.port}), sender6(io, {"::1", 0}), sender4(io, {"127.0.0.1", 0});
    co_await sender6.send_to("v6", {"::1", local.port});
    co_await sender4.send_to("v4", {"127.0.0.1", local.port});
    std::array<char, 16> bytes{};
    require((co_await only_v6.receive_from(bytes)).sender.family == address_family::ipv6, "IPv6-only UDP receives IPv6");
    require((co_await v4.receive_from(bytes)).sender.family == address_family::ipv4, "separate IPv4 UDP port receives IPv4");

    pace::udp::socket dual(io, {"::", 0}, {.ipv6_only = false});
    const auto port = dual.local_endpoint().port;
    co_await sender6.send_to("ipv6", {"::1", port});
    require((co_await dual.receive_from(bytes)).sender.address == "::1", "dual-stack UDP receives IPv6");
    co_await sender4.send_to("ipv4", {"127.0.0.1", port});
    auto packet = co_await dual.receive_from(bytes);
    require(packet.sender.family == address_family::ipv6 && packet.sender.address == "::ffff:127.0.0.1",
            "dual-stack UDP preserves mapped peer");
    co_await dual.send_to("reply", packet.sender);
    require((co_await sender4.receive_from(bytes)).sender.family == address_family::ipv4, "dual-stack UDP mapped reply");
    co_await dual.send_to("mapped", sender4.local_endpoint());
    require((co_await sender4.receive_from(bytes)).size == 6, "dual-stack UDP converts IPv4 destination");
    io.stop();
}

pace::task<void> ipv6_validation(pace::io_context& io) {
    using pace::address_family;
    for (const pace::udp::endpoint& endpoint : {
            pace::udp::endpoint{"::1", 1, address_family::ipv4},
            pace::udp::endpoint{"127.0.0.1", 1, address_family::ipv6},
            pace::udp::endpoint{"127.0.0.1", 1, address_family::unspecified, 1},
            pace::udp::endpoint{"::1%1", 1}, pace::udp::endpoint{"fe80::1%lo", 1},
            pace::udp::endpoint{"[::1]", 1}, pace::udp::endpoint{"::gg", 1},
            pace::udp::endpoint{"::1", 1, static_cast<address_family>(99)}}) {
        bool rejected = false;
        try { pace::udp::socket invalid(io, endpoint); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "invalid UDP bind endpoint accepted");
        pace::udp::socket sender(io);
        rejected = false;
        try { co_await sender.send_to("invalid", endpoint); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "invalid UDP destination accepted");
    }
    pace::udp::socket fixed(io, address_family::ipv4);
    bool rejected = false;
    try { co_await fixed.send_to("wrong family", {"::1", 1}); }
    catch (const std::system_error& error) { rejected = error.code() == std::errc::address_family_not_supported; }
    require(rejected, "explicit IPv4 UDP socket accepted IPv6 destination");
    rejected = false;
    try { pace::udp::socket invalid(io, {"127.0.0.1", 0}, {.ipv6_only = false}); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "dual-stack UDP option on IPv4 accepted");
    io.stop();
}
pace::task<void> receive_wait(pace::udp::socket& socket, bool& canceled) {
    std::array<char, 1> buffer{};
    try { co_await socket.receive_from(buffer); }
    catch (const std::system_error& e) { canceled = e.code() == std::errc::operation_canceled; }
}
pace::task<void> cancel_wait(pace::udp::socket& socket, bool close, bool& duplicate) {
    auto& io = socket.context();
    co_await io.yield();
    std::array<char, 1> buffer{};
    try { co_await socket.receive_from(buffer); }
    catch (const std::logic_error&) { duplicate = true; }
    if (close) socket.close(); else io.stop();
    co_await io.yield();
    io.stop();
}
void run(pace::io_context& io) {
    std::atomic<bool> done{false}, timed_out{false};
    std::thread watchdog([&] {
        for (unsigned i = 0; i < 500 && !done; ++i) std::this_thread::sleep_for(10ms);
        if (!done) { timed_out = true; io.stop(); }
    });
    try { io.run(); } catch (...) { done = true; watchdog.join(); throw; }
    done = true; watchdog.join();
    require(!timed_out, "UDP test watchdog timed out");
}
}
int main() {
    try {
        { pace::io_context io; io.spawn(owned_packets(io)); run(io); }
        { pace::io_context io; io.spawn(roundtrip(io)); run(io); }
        { pace::io_context io; io.spawn(ipv6_roundtrip(io)); run(io); }
        { pace::io_context io; io.spawn(ipv6_wildcards(io)); run(io); }
        { pace::io_context io; io.spawn(ipv6_validation(io)); run(io); }
        for (const auto* address : {"127.0.0.1", "::1"}) for (const bool close : {false, true}) {
            pace::io_context io;
            pace::udp::socket socket(io, {address, 0});
            bool canceled = false, duplicate = false;
            io.spawn(receive_wait(socket, canceled));
            io.spawn(cancel_wait(socket, close, duplicate));
            run(io); require(canceled && duplicate, "cancel/duplicate receive");
        }
        { pace::io_context io; bool invalid = false;
          try { pace::udp::socket socket(io, {"invalid", 0}); }
          catch (const std::invalid_argument&) { invalid = true; }
          require(invalid, "numeric address validation"); }
        { pace::io_context io; bool invalid = false;
          try { pace::udp::socket socket(io, {std::string("127.0.0.1\0suffix", 16), 0}); }
          catch (const std::invalid_argument&) { invalid = true; }
          require(invalid, "embedded NUL address validation"); }
        std::cout << "UDP IPv4/IPv6, dual-stack endpoints, boundaries, truncation and cancellation passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
