#include <pace/net/udp.hpp>
#include <pace/runtime/run.hpp>
#include "net/socket_address.hpp"
#include "support/pace_test.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace
{
using namespace std::chrono_literals;

using pace::test::require;

pace::task<void> owned_packets(pace::io_context& io)
{
    auto receiver = pace::udp::bind({"127.0.0.1", 0});
    auto sender = pace::udp::open({.context = &io});
    auto pending = sender.send(std::string("a\0b", 3), receiver.local_endpoint());
    co_await io.yield();
    require((co_await std::move(pending)) == 3, "owned UDP lazy send");
    auto packet = co_await receiver.receive(3);
    require(
        packet.data == std::string("a\0b", 3) && !packet.truncated
            && packet.sender.port == sender.local_endpoint().port,
        "owned UDP packet/source"
    );
    co_await receiver.send("reply", packet.sender);
    require((co_await sender.receive()).data == "reply", "owned UDP reply");
    co_await sender.send("", receiver.local_endpoint());
    auto empty = co_await receiver.receive();
    require(empty.data.empty() && !empty.truncated, "empty UDP packet is not EOF");
    co_await sender.send("123456", receiver.local_endpoint());
    auto cut = co_await receiver.receive(3);
    require(cut.data == "123" && cut.truncated, "owned UDP truncation flag");
    for (auto capacity : {std::size_t{0}, std::size_t{65537}})
    {
        bool rejected = false;
        try
        {
            (void)co_await receiver.receive(capacity);
        }
        catch (const std::logic_error&)
        {
            rejected = true;
        }
        require(rejected, "owned receive invalid allocation bound");
    }
    auto timeout = io.call_after(1ms, [&] { receiver.close(); });
    bool canceled = false;
    try
    {
        (void)co_await receiver.receive();
    }
    catch (const std::system_error& error)
    {
        canceled = error.code() == std::errc::operation_canceled;
    }
    require(canceled, "owned receive lost cancellation");
    io.stop();
}

pace::task<void> exchange_replies(std::shared_ptr<pace::udp::socket> peer)
{
    auto request = co_await peer->receive(16);
    require(request.data == "request", "UDP exchange request");
    auto other = pace::udp::bind(peer->context(), {"127.0.0.1", 0});
    co_await other.send_to(pace::as_bytes("unrelated"), request.sender);
    co_await peer->send_to(pace::as_bytes("reply"), request.sender);
}

pace::task<void> exchange_filtered(pace::io_context& io)
{
    auto peer = std::make_shared<pace::udp::socket>(io, pace::udp::endpoint{"127.0.0.1", 0});
    const auto endpoint = peer->local_endpoint();
    io.spawn(exchange_replies(peer));
    auto reply = co_await pace::udp::async_exchange(endpoint, "request", {.context = &io, .max_reply_size = 3});
    require(
        reply.data == "rep" && reply.truncated && reply.sender.port == endpoint.port,
        "UDP exchange must preserve source filtering and truncation"
    );
    io.stop();
}

pace::task<void> roundtrip(pace::io_context& io)
{
    pace::udp::socket receiver(io, {"127.0.0.1", 0}), sender(io, {"127.0.0.1", 0});
    auto destination = receiver.local_endpoint();
    const auto source = sender.local_endpoint();
    sender.set_broadcast();
    sender.set_broadcast(false);
    std::string data("a\0b\xff", 4);
    require(co_await sender.send_to(pace::as_bytes(data), destination) == data.size(), "send length");
    std::array<char, 16> buffer{};
    auto packet = co_await receiver.receive_from(pace::as_writable_bytes(buffer));
    require(
        packet.size == data.size() && !packet.truncated && std::string_view(buffer.data(), packet.size) == data,
        "binary datagram mismatch"
    );
    require(packet.sender.address == source.address && packet.sender.port == source.port, "sender endpoint");
    co_await receiver.send_to(pace::as_bytes("reply"), packet.sender);
    require((co_await sender.receive_from(pace::as_writable_bytes(buffer))).size == 5, "reply");
    co_await sender.send_to(pace::as_bytes(std::string_view{}), destination);
    require(
        (co_await receiver.receive_from(pace::as_writable_bytes(buffer))).size == 0,
        "zero datagram must not mean EOF"
    );
    co_await sender.send_to(pace::as_bytes("123456789"), destination);
    auto cut = co_await receiver.receive_from(pace::as_writable_bytes(std::span<char>(buffer.data(), 3)));
    require(cut.size == 3 && cut.truncated && std::string_view(buffer.data(), 3) == "123", "truncated datagram");
    co_await sender.send_to(pace::as_bytes("next"), destination);
    packet = co_await receiver.receive_from(pace::as_writable_bytes(buffer));
    require(packet.size == 4 && !packet.truncated && std::string_view(buffer.data(), 4) == "next", "datagram boundary");
    std::string oversized(65508, 'x');
    bool rejected = false;
    try
    {
        co_await sender.send_to(pace::as_bytes(oversized), destination);
    }
    catch (const std::system_error& e)
    {
        rejected = e.code() == std::errc::message_size;
    }
    require(rejected, "oversize send");
    rejected = false;
    try
    {
        co_await receiver.receive_from(pace::as_writable_bytes(std::span<char>{}));
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    require(rejected, "empty receive buffer");
    pace::udp::socket moved(std::move(sender));
    require(moved.is_open() && !sender.is_open(), "move transfers ownership");
    moved.close();
    moved.close();
    rejected = false;
    try
    {
        co_await moved.send_to(pace::as_bytes("x"), destination);
    }
    catch (const std::system_error& e)
    {
        rejected = e.code() == std::errc::bad_file_descriptor;
    }
    require(rejected, "closed socket");
    io.stop();
}

pace::task<void> packet_information(pace::io_context& io, bool ipv6)
{
    const std::string address = ipv6 ? "::1" : "127.0.0.1";
    pace::udp::socket listener(io, {ipv6 ? "::" : "0.0.0.0", 0}, {.packet_info = true});
    pace::udp::socket client(io, {address, 0}, {.packet_info = true});
    pace::udp::endpoint destination{address, listener.local_endpoint().port};
    pace::udp::packet_info sent;
#if !defined(_WIN32) || (defined(IP_ECN) && defined(IPV6_ECN))
    sent.ecn = 2;
#endif
    co_await client.send_to(pace::as_bytes("request"), destination, sent);
    pace::udp::packet_info received;
    std::array<std::byte, 16> bytes{};
    const auto request = co_await listener.receive_from(bytes, received);
    require(received.local_address == address, "UDP wildcard destination address");
    require(received.ecn == sent.ecn, "UDP ECN round trip");
    // Reply from the address the client contacted.
    co_await listener.send_to(std::span(bytes).first(request.size), request.sender, received);
    auto reply = co_await client.receive_from(bytes, received);
    require(reply.sender.address == address && reply.size == 7 && received.ecn == sent.ecn, "UDP reply metadata");
    io.stop();
}

pace::task<void> batched_datagrams(pace::io_context& io)
{
    pace::udp::socket receiver(io, {"::1", 0}), sender(io);
    const auto addr = receiver.local_endpoint();
    const std::array<pace::udp::outgoing_datagram, 3> outgoing{
        {{pace::as_bytes("first"), addr}, {{}, addr}, {pace::as_bytes("last"), addr}}
    };
    std::size_t sent = 0;
    while (sent < outgoing.size())
    {
        sent += co_await sender.send_batch(std::span(outgoing).subspan(sent));
    }
    std::array<std::array<std::byte, 4>, 3> storage{};
    std::array<pace::udp::incoming_datagram, 3> slots{{{storage[0]}, {storage[1]}, {storage[2]}}};
    std::size_t read = 0;
    while (read < slots.size())
    {
        read += co_await receiver.receive_batch(std::span(slots).subspan(read));
    }
    require(
        slots[0].result.size == 4 && slots[0].result.truncated && pace::as_text(storage[0]) == "firs",
        "batch truncation"
    );
    require(slots[1].result.size == 0 && !slots[1].result.truncated, "empty batch datagram");
    require(
        slots[2].result.sender.port == sender.local_endpoint().port && pace::as_text(storage[2]) == "last",
        "batch order"
    );
    io.stop();
}

#if defined(__linux__)                                                                                                 \
    || (defined(UDP_SEND_MSG_SIZE) && defined(UDP_RECV_MAX_COALESCED_SIZE) && defined(UDP_COALESCED_INFO))
pace::task<void> segmented_datagrams(pace::io_context& io)
{
    pace::udp::socket receiver(io, {"127.0.0.1", 0}, {.coalesce = true});
    pace::udp::socket sender(io);
    const auto bytes = pace::as_bytes("aaaabbbbcc");
    pace::udp::packet_info segments;
    segments.segment_size = 4;
    co_await sender.send_to(bytes, receiver.local_endpoint(), segments);
    std::array<std::byte, 32> storage{};
    pace::udp::packet_info received;
    std::string payload;
    unsigned datagrams = 0;
    while (payload.size() < bytes.size())
    {
        const auto result = co_await receiver.receive_from(storage, received);
        require(!result.truncated && result.size > 0, "segmented UDP receive");
        if (received.segment_size)
        {
            require(received.segment_size == 4, "GRO segment size");
            datagrams += static_cast<unsigned>((result.size + 3) / 4);
        }
        else
        {
            require(result.size <= 4, "unsplit GSO payload");
            ++datagrams;
        }
        payload += pace::as_text(std::span(storage).first(result.size));
    }
    require(datagrams == 3 && payload == pace::as_text(bytes), "UDP segment boundaries and payload");
    io.stop();
}
#endif

pace::task<void> ipv6_roundtrip(pace::io_context& io)
{
    using pace::address_family;
    pace::udp::socket receiver(io, {"::1", 0}), sender(io);
    const auto destination = receiver.local_endpoint();
    require(
        destination.family == address_family::ipv6 && destination.address == "::1" && destination.port != 0
            && destination.scope_id == 0,
        "IPv6 bound UDP endpoint"
    );
    const std::string payload("IPv6\0UDP", 8);
    co_await sender.send_to(pace::as_bytes(payload), destination); // First send chooses IPv6.
    const auto source = sender.local_endpoint();
    require(source.family == address_family::ipv6 && source.port != 0, "IPv6 UDP automatic bind");
    std::array<char, 32> bytes{};
    auto packet = co_await receiver.receive_from(pace::as_writable_bytes(bytes));
    require(
        packet.sender.family == address_family::ipv6 && packet.sender.address == "::1"
            && packet.sender.port == source.port && packet.sender.scope_id == 0 && packet.size == payload.size()
            && std::string_view(bytes.data(), packet.size) == payload,
        "IPv6 UDP payload and source inspection"
    );
    co_await receiver.send_to(pace::as_bytes("reply"), packet.sender);
    require((co_await sender.receive_from(pace::as_writable_bytes(bytes))).size == 5, "IPv6 UDP reply");
    co_await sender.send_to(pace::as_bytes(std::string_view{}), destination);
    require((co_await receiver.receive_from(pace::as_writable_bytes(bytes))).size == 0, "IPv6 empty datagram");
    co_await sender.send_to(pace::as_bytes("truncated"), destination);
    packet = co_await receiver.receive_from(pace::as_writable_bytes(std::span<char>(bytes).first(3)));
    require(
        packet.truncated && packet.size == 3 && packet.sender.family == address_family::ipv6,
        "IPv6 datagram truncation"
    );
    bool rejected = false;
    try
    {
        co_await sender.send_to(pace::as_bytes("wrong family"), {"127.0.0.1", destination.port});
    }
    catch (const std::system_error& error)
    {
        rejected = error.code() == std::errc::address_family_not_supported;
    }
    require(rejected, "selected UDP socket family must not change");
    rejected = false;
    try
    {
        receiver.set_broadcast();
    }
    catch (const std::system_error& error)
    {
        rejected = error.code() == std::errc::operation_not_supported;
    }
    require(rejected, "IPv6 UDP broadcast must be rejected");
    pace::udp::socket moved(std::move(sender));
    pace::udp::socket assigned(io);
    assigned = std::move(moved);
    require(
        assigned.local_endpoint().family == address_family::ipv6 && assigned.local_endpoint().port == source.port,
        "IPv6 UDP move ownership"
    );
    co_await assigned.send_to(pace::as_bytes("moved"), destination);
    require((co_await receiver.receive_from(pace::as_writable_bytes(bytes))).size == 5, "IPv6 UDP send after move");
    pace::udp::socket fixed(io, address_family::ipv6);
    const auto handle = fixed.native_handle();
    fixed.bind({"::1", 0});
    require(fixed.native_handle() == handle, "explicit UDP family changed native handle");
    co_await fixed.send_to(pace::as_bytes("fixed"), destination);
    require((co_await receiver.receive_from(pace::as_writable_bytes(bytes))).size == 5, "explicit IPv6 UDP socket");
    io.stop();
}

pace::task<void> ipv6_wildcards(pace::io_context& io)
{
    using pace::address_family;
    pace::udp::socket only_v6(io, {"::", 0});
    const auto local = only_v6.local_endpoint();
    require(local.address == "::" && local.family == address_family::ipv6, "IPv6 UDP wildcard");
    int only = 0;
    pace::net::socket_address_length length = sizeof(only);
    require(
        ::getsockopt(only_v6.native_handle(), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&only), &length) == 0
            && only == 1,
        "UDP IPv6-only default"
    );
    pace::udp::socket v4(io, {"0.0.0.0", local.port}), sender6(io, {"::1", 0}), sender4(io, {"127.0.0.1", 0});
    co_await sender6.send_to(pace::as_bytes("v6"), {"::1", local.port});
    co_await sender4.send_to(pace::as_bytes("v4"), {"127.0.0.1", local.port});
    std::array<char, 16> bytes{};
    require(
        (co_await only_v6.receive_from(pace::as_writable_bytes(bytes))).sender.family == address_family::ipv6,
        "IPv6-only UDP receives IPv6"
    );
    require(
        (co_await v4.receive_from(pace::as_writable_bytes(bytes))).sender.family == address_family::ipv4,
        "separate IPv4 UDP port receives IPv4"
    );

    pace::udp::socket dual(io, {"::", 0}, {.ipv6_only = false});
    const auto port = dual.local_endpoint().port;
    co_await sender6.send_to(pace::as_bytes("ipv6"), {"::1", port});
    require(
        (co_await dual.receive_from(pace::as_writable_bytes(bytes))).sender.address == "::1",
        "dual-stack UDP receives IPv6"
    );
    co_await sender4.send_to(pace::as_bytes("ipv4"), {"127.0.0.1", port});
    auto packet = co_await dual.receive_from(pace::as_writable_bytes(bytes));
    require(
        packet.sender.family == address_family::ipv6 && packet.sender.address == "::ffff:127.0.0.1",
        "dual-stack UDP preserves mapped peer"
    );
    co_await dual.send_to(pace::as_bytes("reply"), packet.sender);
    require(
        (co_await sender4.receive_from(pace::as_writable_bytes(bytes))).sender.family == address_family::ipv4,
        "dual-stack UDP mapped reply"
    );
    co_await dual.send_to(pace::as_bytes("mapped"), sender4.local_endpoint());
    require(
        (co_await sender4.receive_from(pace::as_writable_bytes(bytes))).size == 6,
        "dual-stack UDP converts IPv4 destination"
    );
    io.stop();
}

pace::task<void> ipv6_validation(pace::io_context& io)
{
    using pace::address_family;
    for (const pace::udp::endpoint& endpoint :
         {pace::udp::endpoint{"::1", 1, address_family::ipv4},
          pace::udp::endpoint{"127.0.0.1", 1, address_family::ipv6},
          pace::udp::endpoint{"127.0.0.1", 1, address_family::unspecified, 1},
          pace::udp::endpoint{"::1%1", 1},
          pace::udp::endpoint{"fe80::1%lo", 1},
          pace::udp::endpoint{"[::1]", 1},
          pace::udp::endpoint{"::gg", 1},
          pace::udp::endpoint{"::1", 1, static_cast<address_family>(99)}})
    {
        bool rejected = false;
        try
        {
            pace::udp::socket invalid(io, endpoint);
        }
        catch (const std::invalid_argument&)
        {
            rejected = true;
        }
        require(rejected, "invalid UDP bind endpoint accepted");
        pace::udp::socket sender(io);
        rejected = false;
        try
        {
            co_await sender.send_to(pace::as_bytes("invalid"), endpoint);
        }
        catch (const std::invalid_argument&)
        {
            rejected = true;
        }
        require(rejected, "invalid UDP destination accepted");
    }
    pace::udp::socket fixed(io, address_family::ipv4);
    bool rejected = false;
    try
    {
        co_await fixed.send_to(pace::as_bytes("wrong family"), {"::1", 1});
    }
    catch (const std::system_error& error)
    {
        rejected = error.code() == std::errc::address_family_not_supported;
    }
    require(rejected, "explicit IPv4 UDP socket accepted IPv6 destination");
    rejected = false;
    try
    {
        pace::udp::socket invalid(io, {"127.0.0.1", 0}, {.ipv6_only = false});
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    require(rejected, "dual-stack UDP option on IPv4 accepted");
    io.stop();
}

pace::task<void> receive_wait(pace::udp::socket& socket, bool& canceled)
{
    std::array<char, 1> buffer{};
    try
    {
        co_await socket.receive_from(pace::as_writable_bytes(buffer));
    }
    catch (const std::system_error& e)
    {
        canceled = e.code() == std::errc::operation_canceled;
    }
}

pace::task<void> cancel_wait(pace::udp::socket& socket, bool close, bool& duplicate)
{
    auto& io = socket.context();
    co_await io.yield();
    std::array<char, 1> buffer{};
    try
    {
        co_await socket.receive_from(pace::as_writable_bytes(buffer));
    }
    catch (const std::logic_error&)
    {
        duplicate = true;
    }
    if (close)
    {
        socket.close();
    }
    else
    {
        io.stop();
    }
    co_await io.yield();
    io.stop();
}

void run(pace::io_context& io)
{
    std::atomic<bool> done{false}, timed_out{false};
    std::thread watchdog(
        [&]
        {
            for (unsigned i = 0; i < 500 && !done; ++i)
            {
                std::this_thread::sleep_for(10ms);
            }
            if (!done)
            {
                timed_out = true;
                io.stop();
            }
        }
    );
    try
    {
        io.run();
    }
    catch (...)
    {
        done = true;
        watchdog.join();
        throw;
    }
    done = true;
    watchdog.join();
    require(!timed_out, "UDP test watchdog timed out");
}
} // namespace

TEST_CASE("owned UDP sockets send, receive, truncate and cancel")
{
    pace::io_context io;
    io.spawn(owned_packets(io));
    run(io);
}

TEST_CASE("UDP exchange ignores packets from other peers")
{
    pace::io_context io;
    io.spawn(exchange_filtered(io));
    run(io);
}

TEST_CASE("UDP IPv4 round trip")
{
    pace::io_context io;
    io.spawn(roundtrip(io));
    run(io);
}

TEST_CASE("UDP IPv6 round trip")
{
    pace::io_context io;
    io.spawn(ipv6_roundtrip(io));
    run(io);
}

TEST_CASE("UDP IPv6 wildcard endpoints")
{
    pace::io_context io;
    io.spawn(ipv6_wildcards(io));
    run(io);
}

TEST_CASE("UDP IPv6 endpoint validation")
{
    pace::io_context io;
    io.spawn(ipv6_validation(io));
    run(io);
}

TEST_CASE("UDP receive cancellation and duplicate receive")
{
    for (const auto* address : {"127.0.0.1", "::1"})
    {
        for (const bool close : {false, true})
        {
            CAPTURE(address);
            CAPTURE(close);
            pace::io_context io;
            pace::udp::socket socket(io, {address, 0});
            bool canceled = false, duplicate = false;
            io.spawn(receive_wait(socket, canceled));
            io.spawn(cancel_wait(socket, close, duplicate));
            run(io);
            CHECK(canceled);
            CHECK(duplicate);
        }
    }
}

TEST_CASE("UDP rejects non-numeric and NUL-embedded addresses")
{
    pace::io_context io;
    CHECK_THROWS_AS(pace::udp::socket(io, {"invalid", 0}), std::invalid_argument);
    CHECK_THROWS_AS(pace::udp::socket(io, {std::string("127.0.0.1\0suffix", 16), 0}), std::invalid_argument);
}

TEST_CASE("UDP wildcard listeners receive destination address and ECN and select the reply source")
{
    for (bool ipv6 : {false, true})
    {
        CAPTURE(ipv6);
        pace::io_context io;
        io.spawn(packet_information(io, ipv6));
        run(io);
    }
}

TEST_CASE("UDP batches preserve datagram boundaries, sender and truncation")
{
    pace::io_context io;
    io.spawn(batched_datagrams(io));
    run(io);
}

#if defined(__linux__)                                                                                                 \
    || (defined(UDP_SEND_MSG_SIZE) && defined(UDP_RECV_MAX_COALESCED_SIZE) && defined(UDP_COALESCED_INFO))
TEST_CASE("UDP segmentation and coalescing preserve datagrams")
{
    pace::io_context io;
    io.spawn(segmented_datagrams(io));
#ifdef _WIN32
    try
    {
        run(io);
    }
    catch (const std::system_error& error)
    {
        if (error.code().category() != std::system_category()
            || (error.code().value() != WSAENOPROTOOPT && error.code().value() != WSAEOPNOTSUPP))
        {
            throw;
        }
        MESSAGE("UDP segmentation unavailable from this Winsock provider: " << error.what());
    }
#else
    run(io);
#endif
}
#endif

static_assert(std::same_as<decltype(std::declval<pace::udp::socket&>().send_to({}, {}, {})), pace::task<std::size_t>>);

TEST_CASE("UDP result overloads distinguish empty and truncated datagrams from errors")
{
    pace::run(
        [](pace::io_context& io) -> pace::task<void>
        {
            using pace::as_result;
            auto receiver = pace::udp::bind(io, {"127.0.0.1", 0});
            auto sender = pace::udp::open({.context = &io});
            auto address = receiver.local_endpoint();
            std::array<std::byte, 1> byte{};
            auto sent = co_await sender.send_to(as_result, {}, address);
            require(sent && sent.value() == 0, "successful zero-byte UDP send");
            auto empty = co_await receiver.receive_from(as_result, byte);
            require(empty && !empty.value().truncated && empty.value().size == 0, "successful empty datagram");
            require(empty.value().sender.port == sender.local_endpoint().port, "empty datagram sender");

            auto invalid = co_await receiver.receive_from(as_result, {});
            require(!invalid && invalid.error() == std::errc::invalid_argument, "empty receive buffer result");
            std::string oversized(65508, 'x');
            auto too_large = co_await sender.send_to(as_result, pace::as_bytes(oversized), address);
            require(!too_large && too_large.error() == std::errc::message_size, "oversized UDP send result");

            auto longer = co_await sender.send_to(as_result, pace::as_bytes("ab"), address, {});
            require(longer && longer.value() == 2, "metadata UDP send result");
            pace::udp::packet_info info;
            auto cut = co_await receiver.receive_from(as_result, byte, info);
            require(
                cut && cut.value().truncated && cut.value().size == 1 && byte[0] == std::byte{'a'},
                "UDP truncation result"
            );
            auto closer = io.call_after(5ms, [&] { receiver.close(); });
            auto canceled = co_await receiver.receive_from(as_result, byte);
            require(!canceled && canceled.error() == std::errc::operation_canceled, "suspended UDP receive result");
            auto closed = co_await receiver.receive_from(as_result, byte);
            require(!closed && closed.error() == std::errc::bad_file_descriptor, "closed UDP receive result");
        }
    );
}
