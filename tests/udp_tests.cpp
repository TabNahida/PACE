#include <pace/udp.hpp>
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
        { pace::io_context io; io.spawn(roundtrip(io)); run(io); }
        for (const bool close : {false, true}) {
            pace::io_context io;
            pace::udp::socket socket(io, {"127.0.0.1", 0});
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
        std::cout << "UDP datagrams, endpoints, boundaries, truncation and cancellation passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
