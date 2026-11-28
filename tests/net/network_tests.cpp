#include <pace/net/tcp.hpp>
#include <pace/pace.hpp>
#include "net/socket_address.hpp"
#include "support/pace_test.hpp"
#ifndef _WIN32
#include <net/if.h>
#include <netinet/tcp.h>
#else
#include <windows.h>
#endif

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace
{
using namespace std::chrono_literals;

using pace::test::require;

pace::task<void> echo_connection(pace::tcp::stream stream)
{
    try
    {
        std::array<char, 4096> buffer{};
        while (const auto size = co_await stream.read_some(pace::as_writable_bytes(buffer)))
        {
            co_await stream.write_all(pace::as_bytes(std::string_view(buffer.data(), size)));
        }
    }
    catch (const std::system_error& error)
    {
        if (error.code() != std::errc::operation_canceled)
        {
            throw;
        }
    }
}

pace::task<void> echo_listener(std::shared_ptr<pace::tcp::listener> listener)
{
    auto stream = co_await listener->accept();
    co_await echo_connection(std::move(stream));
}

pace::task<void> tcp_roundtrip(pace::io_context& context)
{
    auto listener = std::make_shared<pace::tcp::listener>(context, pace::tcp::endpoint{"127.0.0.1", 0});
    context.spawn(echo_listener(listener));
    pace::tcp::stream client(context);
    co_await client.connect(listener->local_endpoint());
    client.set_no_delay();
    std::string payload(96 * 1024, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i)
    {
        payload[i] = static_cast<char>(i % 251);
    }
    co_await client.write_all(pace::as_bytes(payload));
    std::string received;
    std::array<char, 3071> buffer{};
    while (received.size() < payload.size())
    {
        const auto size = co_await client.read_some(pace::as_writable_bytes(buffer));
        require(size != 0, "unexpected echo EOF");
        received.append(buffer.data(), size);
    }
    require(received == payload, "binary TCP roundtrip mismatch");
    pace::tcp::stream legacy(std::move(client));
    pace::tcp::stream moved(std::move(legacy));
    require(
        !client.is_open() && !legacy.is_open() && moved.is_open(),
        "new and compatibility APIs share socket ownership"
    );
    moved.close();
    moved.close();
    bool bad_fd = false;
    try
    {
        co_await moved.read_some(pace::as_writable_bytes(buffer));
    }
    catch (const std::system_error& error)
    {
        bad_fd = error.code() == std::errc::bad_file_descriptor;
    }
    require(bad_fd, "closed stream should reject operations");
}

pace::task<void> adopt_and_configure(pace::io_context& io)
{
    pace::initialize_sockets();
    const auto local = pace::net::parse_socket_address("127.0.0.1", 0);
    const auto server_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(server_fd != pace::invalid_socket, "raw TCP listener socket");
    require(::bind(server_fd, local.data(), local.length) == 0 && ::listen(server_fd, 4) == 0, "raw TCP listen");
    CHECK_THROWS_AS(pace::tcp::stream::adopt(io, server_fd), std::invalid_argument);
    auto listener = pace::tcp::listener::adopt(io, server_fd);
    const auto client_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK_THROWS_AS(pace::tcp::listener::adopt(io, client_fd), std::invalid_argument);
    auto client = pace::tcp::stream::adopt(io, client_fd);

    int calls = 0;
    pace::tcp::connect_options options;
    options.socket.keepalive = true;
    options.pre_connect = [&](pace::native_socket_type fd)
    {
        ++calls;
        require(fd == client_fd, "adopted socket was replaced");
        int enabled = 0;
        pace::net::socket_address_length size = sizeof(enabled);
        require(
            ::getsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<char*>(&enabled), &size) == 0 && enabled,
            "socket options must precede pre_connect"
        );
        pace::net::socket_address peer;
        require(::getpeername(fd, peer.data(), &peer.length) != 0, "hook must precede connect");
    };
    co_await client.connect(listener.local_endpoint(), std::move(options));
    require(calls == 1, "pre_connect must run exactly once");
    const auto accepted = ::accept(server_fd, nullptr, nullptr);
    auto peer = pace::tcp::stream::adopt(io, accepted);
    co_await pace::write(client, "request");
    require((co_await peer.read_exact(7)) == "request", "adopted TCP read");
    co_await pace::write(peer, "reply");
    require((co_await client.read_exact(5)) == "reply", "adopted TCP reply");

    const auto datagram_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK_THROWS_AS(pace::tcp::stream::adopt(io, datagram_fd), std::invalid_argument);
    require(::bind(datagram_fd, local.data(), local.length) == 0, "raw UDP bind");
    auto receiver = pace::udp::socket::adopt(io, datagram_fd);
    pace::udp::socket sender(io);
    co_await sender.send("one", receiver.local_endpoint());
    const auto packet = co_await receiver.receive();
    co_await receiver.send(packet.data, packet.sender);
    require((co_await sender.receive()).data == "one", "adopted UDP echo");
}

pace::task<void> connect_hook_failure(pace::io_context& io)
{
    auto listener = pace::tcp::listen(io, {"127.0.0.1", 0}, {.socket = {.keepalive = true}});
    pace::tcp::stream client(io);
    pace::tcp::connect_options options;
    options.pre_connect = [](pace::native_socket_type) { throw std::runtime_error("hook refused"); };
    try
    {
        co_await client.connect(listener.local_endpoint(), std::move(options));
        require(false, "throwing hook did not abort connect");
    }
    catch (const std::runtime_error& e)
    {
        require(std::string_view(e.what()) == "hook refused", "hook exception lost");
    }
    // A successful connection must be the first one in the listener queue.
    auto next = co_await pace::tcp::connect(listener.local_endpoint());
    auto accepted = co_await listener.accept();
    require(accepted.remote_endpoint().port == next.local_endpoint().port, "failed hook still connected");
}

int read_socket_option(pace::native_socket_type fd, int level, int option)
{
    int value = 0;
    pace::net::socket_address_length length = sizeof(value);
    require(::getsockopt(fd, level, option, reinterpret_cast<char*>(&value), &length) == 0, "read socket option");
    return value;
}

pace::task<void> verify_tcp_options(pace::io_context& io)
{
    pace::socket_options options;
    options.keepalive = true;
#if defined(TCP_KEEPIDLE) || (defined(__APPLE__) && defined(TCP_KEEPALIVE))
    options.keepalive_idle_seconds = 61;
#endif
#if defined(SO_USER_COOKIE)
    options.mark = 42;
#endif
#if defined(TCP_KEEPINTVL)
    options.keepalive_interval_seconds = 17;
#endif
#if defined(TCP_KEEPCNT)
    options.keepalive_count = 5;
#endif
#if defined(TCP_USER_TIMEOUT)
    options.user_timeout_ms = 23100;
#endif
#if defined(SO_SETFIB)
    options.routing_table = 0;
#endif
    const auto verify = [](pace::native_socket_type fd)
    {
        require(read_socket_option(fd, SOL_SOCKET, SO_KEEPALIVE) != 0, "keepalive enabled");
#if defined(TCP_KEEPIDLE)
        require(read_socket_option(fd, IPPROTO_TCP, TCP_KEEPIDLE) == 61, "keepalive idle duration");
#elif defined(__APPLE__) && defined(TCP_KEEPALIVE)
        require(read_socket_option(fd, IPPROTO_TCP, TCP_KEEPALIVE) == 61, "keepalive idle duration");
#endif
#if defined(SO_USER_COOKIE)
        require(read_socket_option(fd, SOL_SOCKET, SO_USER_COOKIE) == 42, "socket user cookie");
#endif
#if defined(TCP_KEEPINTVL)
        require(read_socket_option(fd, IPPROTO_TCP, TCP_KEEPINTVL) == 17, "keepalive interval");
#endif
#if defined(TCP_KEEPCNT)
        require(read_socket_option(fd, IPPROTO_TCP, TCP_KEEPCNT) == 5, "keepalive probes");
#endif
#if defined(TCP_USER_TIMEOUT)
        require(read_socket_option(fd, IPPROTO_TCP, TCP_USER_TIMEOUT) == 23100, "user timeout");
#endif
#if defined(SO_SETFIB)
        require(read_socket_option(fd, SOL_SOCKET, SO_SETFIB) == 0, "routing table");
#endif
    };
    auto server = pace::tcp::listen(io, {"127.0.0.1", 0}, {.socket = options});
    verify(server.native_handle());
    pace::tcp::connect_options client_options;
    client_options.socket = options;
    client_options.pre_connect = verify;
    pace::tcp::stream client(io);
    co_await client.connect(server.local_endpoint(), std::move(client_options));
    auto peer = co_await server.accept();
    require(peer.remote_endpoint().port == client.local_endpoint().port, "connection with native options");
}

struct native_option
{
    int level, name, value;
};

native_option df_option(bool v6)
{
#if defined(IP_MTU_DISCOVER) && (defined(IP_PMTUDISC_DO) || defined(_WIN32))
    return {v6 ? IPPROTO_IPV6 : IPPROTO_IP, v6 ? IPV6_MTU_DISCOVER : IP_MTU_DISCOVER, IP_PMTUDISC_DO};
#elif defined(IP_DONTFRAG)
    return {v6 ? IPPROTO_IPV6 : IPPROTO_IP, v6 ? IPV6_DONTFRAG : IP_DONTFRAG, 1};
#else
    return {v6 ? IPPROTO_IPV6 : IPPROTO_IP, v6 ? IPV6_DONTFRAG : IP_DONTFRAGMENT, 1};
#endif
}

pace::task<void> verify_routing_options(pace::io_context& io)
{
    for (bool v6 : {false, true})
    {
        const char* address = v6 ? "::1" : "127.0.0.1";
        pace::socket_options options;
        const auto df = df_option(v6);
        pace::tcp::stream probe(io, v6 ? pace::address_family::ipv6 : pace::address_family::ipv4);
        if (::setsockopt(
                probe.native_handle(),
                df.level,
                df.name,
                reinterpret_cast<const char*>(&df.value),
                sizeof(df.value)
            )
            == 0)
        {
            options.dont_fragment = true;
        }
        else
        {
            // Some stacks (including Darwin IPv6) expose DF only for datagram sockets.
#ifdef _WIN32
            const std::error_code expected(WSAGetLastError(), std::system_category());
#else
            const std::error_code expected(errno, std::generic_category());
#endif
            const auto result =
                co_await probe.connect(pace::as_result, {address, 0}, {.socket = {.dont_fragment = true}});
            require(result.error() == expected, "TCP DF preserves the native error");
        }
#if defined(IP_BOUND_IF) || defined(_WIN32)
        // Observe the loopback interface selected by this host; indices are not portable.
        pace::udp::socket receiver(io, {address, 0}, {.packet_info = true}), sender(io);
        co_await sender.send("i", receiver.local_endpoint());
        std::array<std::byte, 1> byte{};
        pace::udp::packet_info info;
        co_await receiver.receive_from(byte, info);
        require(info.interface_index != 0, "loopback interface from packet information");
        options.interface_index = info.interface_index;
#endif
        const auto verify = [&](pace::native_socket_type fd)
        {
            if (options.dont_fragment)
            {
                require(read_socket_option(fd, df.level, df.name) == df.value, "TCP socket DF/PMTU policy");
            }
#if defined(IP_BOUND_IF)
            require(
                unsigned(read_socket_option(fd, v6 ? IPPROTO_IPV6 : IPPROTO_IP, v6 ? IPV6_BOUND_IF : IP_BOUND_IF))
                    == *options.interface_index,
                "bound interface"
            );
#elif defined(_WIN32)
            const auto index =
                unsigned(read_socket_option(fd, v6 ? IPPROTO_IPV6 : IPPROTO_IP, v6 ? IPV6_UNICAST_IF : IP_UNICAST_IF));
            require((v6 ? index : ntohl(index)) == *options.interface_index, "unicast interface");
#endif
        };
        auto server = pace::tcp::listen(io, {address, 0}, {.socket = options});
        verify(server.native_handle());
        auto client = co_await pace::tcp::connect(
            server.local_endpoint(),
            {.transport = {.socket = options, .pre_connect = verify}}
        );
        auto peer = co_await server.accept();
        require(client.local_endpoint().port == peer.remote_endpoint().port, "connection followed selected route");
    }
}

#if defined(TCP_FASTOPEN)
pace::task<void> fast_open_exchange(pace::io_context& io)
{
    // Some kernels disable server TFO by policy. Probe the native option on a
    // listening socket so an error in the library's setup order still fails the test.
    auto probe = pace::tcp::listen(io, {"127.0.0.1", 0});
    const int on = 1;
    const bool server_tfo =
        ::setsockopt(probe.native_handle(), IPPROTO_TCP, TCP_FASTOPEN, reinterpret_cast<const char*>(&on), sizeof(on))
        == 0;
#if defined(__FreeBSD__)
    pace::tcp::stream client_probe(io);
    if (::setsockopt(
            client_probe.native_handle(),
            IPPROTO_TCP,
            TCP_FASTOPEN,
            reinterpret_cast<const char*>(&on),
            sizeof(on)
        )
        != 0)
    {
        if (errno != EPERM && errno != ENOPROTOOPT && errno != EOPNOTSUPP)
        {
            throw std::system_error(errno, std::generic_category(), "probe client Fast Open");
        }
        MESSAGE("FreeBSD kernel has disabled TCP Fast Open");
        co_return;
    }
#endif
    auto listener = std::make_shared<pace::tcp::listener>(
        io,
        pace::tcp::endpoint{"127.0.0.1", 0},
        pace::tcp::listen_options{.socket = {.fast_open = server_tfo}}
    );
    if (server_tfo)
    {
        int enabled = 0;
        pace::net::socket_address_length length = sizeof(enabled);
        require(
            ::getsockopt(
                listener->native_handle(),
                IPPROTO_TCP,
                TCP_FASTOPEN,
                reinterpret_cast<char*>(&enabled),
                &length
            ) == 0
                && enabled,
            "listener enabled TCP Fast Open"
        );
    }
    io.spawn(echo_listener(listener));
    // Longer than the first ConnectEx/sendto write. Check every echoed byte.
    std::string request(48 * 1024, 'x');
    for (std::size_t i = 0; i < request.size(); ++i)
    {
        request[i] = static_cast<char>(i % 251);
    }
    auto reply = co_await pace::tcp::async_exchange(
        listener->local_endpoint(),
        request,
        request.size(),
        {.timeout = 5s, .transport = {.socket = {.fast_open = true}}}
    );
    require(reply == request, "Fast Open exchange lost or duplicated data");
}
#endif

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
pace::task<void> check_redirect(pace::tcp::listener& listener, pace::tcp::endpoint target)
{
    auto client = co_await pace::tcp::connect(target);
    auto accepted = co_await listener.accept();
    const auto original = accepted.original_destination();
    require(accepted.local_endpoint().port != target.port, "connection was redirected");
    require(
        original.port == target.port && original.address == target.address && original.family == target.family,
        "original destination must match the address on the wire"
    );
}

pace::task<void> original_destinations(pace::io_context& io)
{
    auto v4 = pace::tcp::listen(io, {"127.0.0.1", 29002});
    co_await check_redirect(v4, {"127.0.0.1", 29001, pace::address_family::ipv4});
    auto dual = pace::tcp::listen(io, {"::", 29006}, {.ipv6_only = false});
    co_await check_redirect(dual, {"127.0.0.1", 29005, pace::address_family::ipv4});
    co_await check_redirect(dual, {"::1", 29005, pace::address_family::ipv6});

#if defined(__linux__)
    const auto index = if_nametoindex("lo");
    require(index != 0, "private network namespace loopback");
    auto marked = pace::tcp::listen(
        io,
        {"127.0.0.1", 0},
        {.socket = {.interface_index = index, .mark = 42, .transparent = true}}
    );
    require(read_socket_option(marked.native_handle(), SOL_SOCKET, SO_MARK) == 42, "SO_MARK");
    require(read_socket_option(marked.native_handle(), IPPROTO_IP, IP_TRANSPARENT) != 0, "IP_TRANSPARENT");
    char device[IF_NAMESIZE]{};
    socklen_t length = sizeof(device);
    require(
        ::getsockopt(marked.native_handle(), SOL_SOCKET, SO_BINDTODEVICE, device, &length) == 0
            && std::string_view(device) == "lo",
        "SO_BINDTODEVICE"
    );
    pace::udp::socket receiver(io, {"0.0.0.0", 29004}, {.transparent = true});
    pace::udp::socket sender(io);
    co_await sender.send("TPROXY", {"127.0.0.2", 29003});
    std::array<std::byte, 16> bytes{};
    pace::udp::packet_info info;
    const auto packet = co_await receiver.receive_from(bytes, info);
    require(
        packet.size == 6 && info.original_destination && info.original_destination->address == "127.0.0.2"
            && info.original_destination->port == 29003,
        "UDP TPROXY original destination"
    );
#endif
}
#endif

pace::task<void> relay_exchange(pace::io_context& io)
{
    auto listener = pace::tcp::listen(io, {"127.0.0.1", 0});
    auto client = co_await pace::tcp::connect(listener.local_endpoint(), {.context = &io});
    auto left = co_await listener.accept();
    auto right = co_await pace::tcp::connect(listener.local_endpoint(), {.context = &io});
    auto server = co_await listener.accept();

    struct observer
    {
        pace::io_context& io;
        std::size_t& seen;

        pace::task<void> before_write(pace::relay_direction, std::size_t bytes)
        {
            co_await io.sleep_for(1ms);
            seen += bytes;
        }
    };

    std::size_t seen = 0;
    co_await pace::with_task_group(
        [&](pace::task_group& group) -> pace::task<void>
        {
            auto pumping =
                group.spawn(pace::relay(left, right, {.idle_timeout = 2s, .linger = 2s}, observer{io, seen}));
            co_await pace::write(client, "request");
            co_await client.shutdown_write();
            require((co_await server.read_exact(7)) == "request", "relay request");
            std::array<std::byte, 1> byte{};
            require((co_await server.read_some(byte)) == 0, "relay forwarded FIN");
            co_await pace::write(server, "reply");
            co_await server.shutdown_write();
            require((co_await client.read_exact(5)) == "reply", "relay kept reverse direction");
            require((co_await client.read_some(byte)) == 0, "relay reverse FIN");
            const auto result = co_await pumping;
            require(result.a_to_b == 7 && result.b_to_a == 5 && seen == 12, "relay observer and counters");
        }
    );
}

pace::task<void> relay_payloads(pace::io_context& io)
{
    std::string payload(512 * 1024 + 7, 0);
    for (std::size_t i = 0; i < payload.size(); ++i)
    {
        payload[i] = static_cast<char>(i % 251);
    }
    for (bool splice : {false, true})
    {
        auto listener = pace::tcp::listen(io, {"127.0.0.1", 0});
        auto client = co_await pace::tcp::connect(listener.local_endpoint(), {.context = &io});
        auto left = co_await listener.accept();
        auto right = co_await pace::tcp::connect(listener.local_endpoint(), {.context = &io});
        auto server = co_await listener.accept();
        co_await pace::with_task_group(
            [&](pace::task_group& group) -> pace::task<void>
            {
                auto pump = group.spawn(pace::relay(left, right, {.splice = splice}));
                auto writer = group.spawn(
                    [&]() -> pace::task<void>
                    {
                        co_await client.write_all(pace::as_bytes(payload));
                        co_await client.shutdown_write();
                    }
                );
                co_await server.shutdown_write();
                require(co_await server.read_exact(payload.size()) == payload, "relay payload mismatch");
                co_await writer;
                const auto result = co_await pump;
                require(result.a_to_b == payload.size() && !result.b_to_a, "relay payload counters");
            }
        );
    }
}

pace::task<void> relay_pacing(pace::io_context& io)
{
    struct limiter
    {
        pace::io_context& io;
        std::chrono::steady_clock::time_point start;
        std::size_t bytes = 0;

        pace::task<void> before_write(pace::relay_direction direction, std::size_t size)
        {
            if (direction == pace::relay_direction::a_to_b)
            {
                bytes += size;
                // One KiB per millisecond, with no initial burst.
                const auto due = start + std::chrono::milliseconds(bytes / 1024);
                co_await io.sleep_for(due - std::chrono::steady_clock::now());
            }
        }
    };

    auto listener = pace::tcp::listen(io, {"127.0.0.1", 0});
    auto client = co_await pace::tcp::connect(listener.local_endpoint(), {.context = &io});
    auto left = co_await listener.accept();
    auto right = co_await pace::tcp::connect(listener.local_endpoint(), {.context = &io});
    auto server = co_await listener.accept();
    std::string payload(32 * 1024, 'r');
    co_await server.shutdown_write();
    co_await pace::with_task_group(
        [&](pace::task_group& group) -> pace::task<void>
        {
            const auto start = std::chrono::steady_clock::now();
            auto pump = group.spawn(pace::relay(left, right, {}, limiter{io, start}));
            auto writer = group.spawn(
                [&]() -> pace::task<void>
                {
                    co_await client.write_all(pace::as_bytes(payload));
                    co_await client.shutdown_write();
                }
            );
            require((co_await server.read_exact(payload.size())) == payload, "paced relay data");
            require(
                std::chrono::steady_clock::now() >= start + 32ms,
                "relay wrote before the observer released its budget"
            );
            require((co_await pump).a_to_b == payload.size(), "paced relay byte count");
            co_await writer;
        }
    );
}

pace::task<void> relay_cancel(pace::io_context& io)
{
    auto listener = pace::tcp::listen(io, {"127.0.0.1", 0});
    auto client = co_await pace::tcp::connect(listener.local_endpoint(), {.context = &io});
    auto left = co_await listener.accept();
    auto right = co_await pace::tcp::connect(listener.local_endpoint(), {.context = &io});
    auto server = co_await listener.accept();
    pace::cancellation_source source;
    auto timer = io.call_after(5ms, [source] { source.request_cancellation(); });
    bool cancelled = false;
    try
    {
        (void)co_await pace::with_cancellation(source.token(), pace::relay(left, right));
    }
    catch (const std::system_error& error)
    {
        cancelled = error.code() == std::errc::operation_canceled;
    }
    require(cancelled && !left.is_open() && !right.is_open(), "relay external cancellation");
}

pace::task<void> relay_timeouts(pace::io_context& io)
{
    for (bool half_close : {false, true})
    {
        auto listener = pace::tcp::listen(io, {"127.0.0.1", 0});
        auto client = co_await pace::tcp::connect(listener.local_endpoint(), {.context = &io});
        auto left = co_await listener.accept();
        auto right = co_await pace::tcp::connect(listener.local_endpoint(), {.context = &io});
        auto server = co_await listener.accept();
        pace::relay_options options;
        if (half_close)
        {
            options.linger = 5ms;
            co_await client.shutdown_write();
        }
        else
        {
            options.idle_timeout = 5ms;
        }
        bool expired = false;
        try
        {
            (void)co_await pace::relay(left, right, options);
        }
        catch (const std::system_error& error)
        {
            expired = error.code() == std::errc::timed_out;
        }
        require(expired && !left.is_open() && !right.is_open(), "relay timeout closes both streams");
    }
}

pace::task<void> accept_burst(pace::io_context& io)
{
    auto listener = pace::tcp::listen(io, {"127.0.0.1", 0});
    std::vector<pace::tcp::stream> clients;
    for (unsigned i = 0; i < 65; ++i)
    {
        clients.push_back(co_await pace::tcp::connect(listener.local_endpoint(), {.context = &io}));
    }
    auto dispatched = std::make_shared<bool>(false);
    io.post([dispatched] { *dispatched = true; });
    for (std::size_t i = 0; i < clients.size(); ++i)
    {
        auto accepted = co_await listener.accept();
        require(accepted.is_open(), "burst accept lost a connection");
#if defined(_WIN32)
        for (auto fd : {listener.native_handle(), clients[i].native_handle(), accepted.native_handle()})
        {
            DWORD flags = 0;
            require(
                ::GetHandleInformation(reinterpret_cast<HANDLE>(fd), &flags) && !(flags & HANDLE_FLAG_INHERIT),
                "TCP socket is inheritable"
            );
        }
#endif
    }
    require(*dispatched, "accept burst did not let the reactor dispatch posted work");
}

pace::task<void> large_write(pace::io_context& io)
{
    auto listener = pace::tcp::listen(io, {"127.0.0.1", 0});
    auto client = co_await pace::tcp::connect(listener.local_endpoint(), {.context = &io});
    auto peer = co_await listener.accept();
    const std::string bytes(2 * 1024 * 1024 + 13, 'x');
    co_await pace::with_task_group(
        [&](pace::task_group& group) -> pace::task<void>
        {
            auto written = group.spawn(client.write_all(pace::as_bytes(bytes)));
            require((co_await peer.read_exact(bytes.size(), bytes.size())) == bytes, "large TCP write lost bytes");
            co_await written;
        }
    );
}

pace::task<void> connect_errors(pace::io_context& context)
{
    pace::tcp::listener reserved(context, {"127.0.0.1", 0});
    auto endpoint = reserved.local_endpoint();
    reserved.close();
    pace::tcp::stream client(context);
    bool failed = false;
    try
    {
        co_await client.connect(endpoint);
    }
    catch (const std::system_error&)
    {
        failed = true;
    }
    require(failed, "connecting to an unbound port should fail");
    bool invalid = false;
    try
    {
        co_await client.connect({"not-an-address", 42});
    }
    catch (const std::invalid_argument&)
    {
        invalid = true;
    }
    require(invalid, "numeric address validation");
    invalid = false;
    pace::tcp::endpoint malformed{std::string("127.0.0.1\0unexpected", 20), 42};
    try
    {
        co_await client.connect(std::move(malformed));
    }
    catch (const std::invalid_argument&)
    {
        invalid = true;
    }
    require(invalid, "embedded NUL address validation");
}

pace::task<void> endpoint_exchange(
    pace::io_context& io,
    pace::tcp::listener& listener,
    pace::tcp::endpoint destination,
    bool explicit_family = false
)
{
    pace::tcp::stream client(io, explicit_family ? pace::address_family::ipv6 : pace::address_family::unspecified);
    client.set_no_delay();
    const auto original_handle = client.native_handle();
    co_await client.connect(std::move(destination));
    if (explicit_family)
    {
        require(client.native_handle() == original_handle, "explicit family changed native handle");
    }
    int no_delay = 0;
    pace::net::socket_address_length option_length = sizeof(no_delay);
    require(
        ::getsockopt(
            client.native_handle(),
            IPPROTO_TCP,
            TCP_NODELAY,
            reinterpret_cast<char*>(&no_delay),
            &option_length
        ) == 0
            && no_delay != 0,
        "TCP_NODELAY was lost while selecting IPv6"
    );
    auto accepted = co_await listener.accept();
    const auto client_local = client.local_endpoint(), client_remote = client.remote_endpoint();
    const auto server_local = accepted.local_endpoint(), server_remote = accepted.remote_endpoint();
    require(client_local.family == client_remote.family && client_local.scope_id == 0, "client endpoint family/scope");
    require(
        server_local.family == listener.local_endpoint().family && server_remote.family == server_local.family,
        "accepted endpoint family"
    );
    require(
        client_remote.port == server_local.port && client_local.port == server_remote.port,
        "local/remote TCP port inspection"
    );
    if (client_local.family != server_remote.family)
    {
        require(
            server_remote.family == pace::address_family::ipv6 && server_remote.address == "::ffff:127.0.0.1",
            "dual-stack peer must retain IPv4-mapped IPv6 identity"
        );
    }
    else
    {
        require(client_local.address == server_remote.address, "TCP peer address inspection");
    }
    const std::string payload("IPv6\0payload", 12);
    co_await client.write_all(pace::as_bytes(payload));
    std::array<char, 64> buffer{};
    std::size_t received = 0;
    while (received < payload.size())
    {
        const auto size =
            co_await accepted.read_some(pace::as_writable_bytes(std::span<char>(buffer).subspan(received)));
        require(size != 0, "IPv6 premature EOF");
        received += size;
    }
    require(std::string_view(buffer.data(), received) == payload, "IPv6 binary transfer");
    co_await accepted.write_all(pace::as_bytes("ok"));
    received = 0;
    while (received < 2)
    {
        const auto size = co_await client.read_some(pace::as_writable_bytes(std::span<char>(buffer).subspan(received)));
        require(size != 0, "IPv6 reply EOF");
        received += size;
    }
    require(std::string_view(buffer.data(), received) == "ok", "IPv6 reply");
    pace::tcp::stream moved(std::move(accepted));
    require(moved.remote_endpoint().address == server_remote.address, "IPv6 move lost peer endpoint");
    moved.close();
    bool closed = false;
    try
    {
        (void)moved.remote_endpoint();
    }
    catch (const std::system_error& error)
    {
        closed = error.code() == std::errc::bad_file_descriptor;
    }
    require(closed, "closed peer endpoint inspection");
}

pace::task<void> ipv6_loopback(pace::io_context& io)
{
    pace::tcp::listener listener(io, {"::1", 0});
    const auto local = listener.local_endpoint();
    require(
        local.address == "::1" && local.port != 0 && local.family == pace::address_family::ipv6 && local.scope_id == 0,
        "IPv6 listener endpoint"
    );
    co_await endpoint_exchange(io, listener, local);
    co_await endpoint_exchange(io, listener, local, true);
    pace::tcp::stream fixed(io, pace::address_family::ipv4);
    bool rejected = false;
    try
    {
        co_await fixed.connect(local);
    }
    catch (const std::system_error& error)
    {
        rejected = error.code() == std::errc::address_family_not_supported;
    }
    require(rejected, "explicit TCP family mismatch");
}

pace::task<void> ipv6_wildcards(pace::io_context& io)
{
    pace::tcp::listener only_v6(io, {"::", 0});
    auto local = only_v6.local_endpoint();
    require(local.address == "::" && local.family == pace::address_family::ipv6, "IPv6 wildcard inspection");
    int only = 0;
    pace::net::socket_address_length length = sizeof(only);
    require(
        ::getsockopt(only_v6.native_handle(), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&only), &length) == 0
            && only == 1,
        "IPv6 listeners must default to IPV6_V6ONLY"
    );
    // A separate IPv4 listener must be able to own the same port.
    pace::tcp::listener v4(io, {"0.0.0.0", local.port});
    co_await endpoint_exchange(io, only_v6, {"::1", local.port});
    co_await endpoint_exchange(io, v4, {"127.0.0.1", local.port});

    pace::tcp::listener dual(io, {"::", 0}, {.ipv6_only = false});
    local = dual.local_endpoint();
    co_await endpoint_exchange(io, dual, {"::1", local.port});
    co_await endpoint_exchange(io, dual, {"127.0.0.1", local.port});
    // Explicit IPv4-mapped numeric destinations also work on an IPv6 stream.
    co_await endpoint_exchange(io, dual, {"::ffff:127.0.0.1", local.port}, true);
}

pace::task<void> ipv6_address_validation(pace::io_context& io)
{
    using pace::address_family;
    for (const pace::tcp::endpoint& endpoint :
         {pace::tcp::endpoint{"::1", 0, address_family::ipv4},
          pace::tcp::endpoint{"127.0.0.1", 0, address_family::ipv6},
          pace::tcp::endpoint{"127.0.0.1", 0, address_family::unspecified, 1},
          pace::tcp::endpoint{"::1%1", 0},
          pace::tcp::endpoint{"fe80::1%lo", 0},
          pace::tcp::endpoint{"[::1]", 0},
          pace::tcp::endpoint{"::gg", 0},
          pace::tcp::endpoint{"::1", 0, static_cast<address_family>(99)},
          pace::tcp::endpoint{std::string("::1\0suffix", 10), 0}})
    {
        bool rejected = false;
        try
        {
            pace::tcp::listener listener(io, endpoint);
        }
        catch (const std::invalid_argument&)
        {
            rejected = true;
        }
        require(rejected, "invalid IPv6 listener endpoint accepted");
        pace::tcp::stream client(io);
        rejected = false;
        try
        {
            co_await client.connect(endpoint);
        }
        catch (const std::invalid_argument&)
        {
            rejected = true;
        }
        require(rejected, "invalid IPv6 connect endpoint accepted");
    }
    bool rejected = false;
    try
    {
        pace::tcp::listener listener(io, {"127.0.0.1", 0}, {.ipv6_only = false});
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    require(rejected, "dual-stack option on IPv4 must be rejected");
    // Link-local reachability depends on host interfaces. The shared conversion
    // must preserve its interface index without a DNS/interface-name lookup.
    const auto native = pace::net::parse_socket_address("fe80::1234", 321, address_family::ipv6, 42);
    const auto scoped = pace::net::socket_endpoint<pace::tcp::endpoint>(native.data(), native.length);
    require(
        scoped.address == "fe80::1234" && scoped.port == 321 && scoped.scope_id == 42
            && scoped.family == address_family::ipv6,
        "IPv6 scope must survive native endpoint conversion"
    );
}

pace::task<void> short_read_server(std::shared_ptr<pace::tcp::listener> listener, std::shared_ptr<bool> tail_sent)
{
    auto stream = co_await listener->accept();
    co_await stream.write_all(pace::as_bytes("A"));
    std::array<char, 1> acknowledgement{};
    require(
        co_await stream.read_some(pace::as_writable_bytes(acknowledgement)) == 1 && acknowledgement[0] == '!',
        "short-read acknowledgement"
    );
    co_await stream.write_all(pace::as_bytes("BC"));
    *tail_sent = true;
}

pace::task<void> short_read_rearm(pace::io_context& context)
{
    auto listener = std::make_shared<pace::tcp::listener>(context, pace::tcp::endpoint{"127.0.0.1", 0});
    auto tail_sent = std::make_shared<bool>(false);
    context.spawn(short_read_server(listener, tail_sent));
    pace::tcp::stream client(context);
    co_await client.connect(listener->local_endpoint());
    std::array<char, 64> buffer{};
    require(co_await client.read_some(pace::as_writable_bytes(buffer)) == 1 && buffer[0] == 'A', "initial short read");
    // A zero-size operation must finish even when the peer is awaiting our ack.
    require(co_await client.read_some(pace::as_writable_bytes(std::span<char>{})) == 0, "empty read after short read");
    pace::tcp::stream moved(std::move(client));
    pace::tcp::stream assigned(context);
    assigned = std::move(moved);
    co_await assigned.write_all(pace::as_bytes("!"));
    while (!*tail_sent)
    {
        co_await context.yield();
    }
    // Data is already queued before rearming. A level subscription must find it;
    // a full one-byte read must also preserve the unread remainder.
    require(
        co_await assigned.read_some(pace::as_writable_bytes(std::span<char>(buffer).first(1))) == 1 && buffer[0] == 'B',
        "queued readiness after short read and moves"
    );
    require(
        co_await assigned.read_some(pace::as_writable_bytes(buffer)) == 1 && buffer[0] == 'C',
        "read remainder after full buffer"
    );
    require(co_await assigned.read_some(pace::as_writable_bytes(buffer)) == 0, "EOF after short read");
}

#ifndef PACE_CORE_ONLY_TESTS
pace::task<pace::http::response> http_handler(pace::http::request request)
{
    require(
        request.header("HOST") == "example.test" || request.version == "HTTP/1.0",
        "case-insensitive header lookup"
    );
    if (request.target == "/bad")
    {
        pace::http::response result{.body = "bad"};
        result.headers.push_back({"X-Injected", "a\r\nContent-Length: 0"});
        co_return result;
    }
    if (request.target == "/empty")
    {
        co_return pace::http::response{.status = 204, .body = "must not appear"};
    }
    if (request.target == "/echo")
    {
        co_return pace::http::response{.body = std::move(request.body)};
    }
    co_return pace::http::response{.body = request.target};
}

pace::task<void> http_listener(std::shared_ptr<pace::tcp::listener> listener, pace::http::limits bounds)
{
    co_await pace::http::serve(*listener, http_handler, bounds);
}

// First validate the advertised HTTP frames, then require orderly closure.
// Reset, truncation and trailing bytes fail even for rejected requests.
bool complete_responses(std::string_view input, std::size_t expected, bool head_response = false)
{
    for (std::size_t index = 0; index < expected; ++index)
    {
        const auto delimiter = input.find("\r\n\r\n");
        if (delimiter == std::string_view::npos)
        {
            return false;
        }
        const auto head = input.substr(0, delimiter + 2);
        require(head.starts_with("HTTP/1.1 ") && head.size() >= 14, "invalid HTTP response status line");
        unsigned status = 0;
        const auto [status_end, status_error] = std::from_chars(head.data() + 9, head.data() + 12, status);
        require(
            status_error == std::errc{} && status_end == head.data() + 12 && head[12] == ' ',
            "invalid HTTP response status code"
        );
        constexpr std::string_view length_header = "\r\nContent-Length: ";
        const auto length_start = head.find(length_header);
        std::size_t length = 0;
        if (length_start != std::string_view::npos)
        {
            const auto value_start = length_start + length_header.size();
            const auto value_end = head.find("\r\n", value_start);
            require(
                value_end != std::string_view::npos && head.find(length_header, value_start) == std::string_view::npos,
                "invalid or duplicate response Content-Length"
            );
            const auto [end, error] = std::from_chars(head.data() + value_start, head.data() + value_end, length);
            require(
                error == std::errc{} && end == head.data() + value_end && length <= 1024 * 1024,
                "invalid response Content-Length"
            );
        }
        const bool bodyless = head_response || status == 204 || status == 304;
        require(bodyless || length_start != std::string_view::npos, "response lacks Content-Length framing");
        input.remove_prefix(delimiter + 4);
        const auto body_size = bodyless ? 0 : length;
        if (input.size() < body_size)
        {
            return false;
        }
        input.remove_prefix(body_size);
    }
    require(input.empty(), "unexpected bytes after the expected HTTP responses");
    return true;
}

void verify_response_framing()
{
    constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    for (std::size_t size = 0; size < response.size(); ++size)
    {
        require(!complete_responses(response.substr(0, size), 1), "truncated HTTP response must remain incomplete");
    }
    require(complete_responses(response, 1), "complete HTTP body must be accepted");
    require(!complete_responses(response, 2), "missing pipelined response must remain incomplete");
    require(complete_responses(std::string(response) + std::string(response), 2), "both pipeline frames are required");
    require(complete_responses("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n", 1, true), "HEAD framing has no payload");
    require(complete_responses("HTTP/1.1 204 No Content\r\n\r\n", 1), "204 framing has no payload");
}

pace::task<std::string> read_responses(pace::tcp::stream& stream, std::size_t expected = 1, bool head_response = false)
{
    std::string output;
    std::array<char, 1024> buffer{};
    while (!complete_responses(output, expected, head_response))
    {
        const auto size = co_await stream.read_some(pace::as_writable_bytes(buffer));
        require(size != 0, "EOF before complete HTTP response");
        output.append(buffer.data(), size);
        require(output.size() <= 1024 * 1024, "HTTP test response exceeds safety bound");
    }
    require(co_await stream.read_some(pace::as_writable_bytes(buffer)) == 0, "unexpected trailing HTTP response bytes");
    co_return output;
}

pace::task<std::string> exchange(
    pace::io_context& context,
    pace::tcp::endpoint endpoint,
    std::string request,
    std::size_t expected = 1
)
{
    pace::tcp::stream stream(context);
    co_await stream.connect(std::move(endpoint));
    co_await stream.write_all(pace::as_bytes(request));
    co_return co_await read_responses(stream, expected, request.starts_with("HEAD "));
}

pace::task<void> http_protocol(pace::io_context& context)
{
    auto listener = std::make_shared<pace::tcp::listener>(context, pace::tcp::endpoint{"127.0.0.1", 0});
    context.spawn(
        http_listener(listener, {.max_header_bytes = 256, .max_body_bytes = 32, .max_requests_per_connection = 2})
    );
    const auto endpoint = listener->local_endpoint();
    auto result = co_await exchange(
        context,
        endpoint,
        "POST /echo HTTP/1.1\r\nHost: example.test\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello"
    );
    require(result.starts_with("HTTP/1.1 200") && result.ends_with("\r\n\r\nhello"), "HTTP body roundtrip");

    result = co_await exchange(
        context,
        endpoint,
        "GET /one HTTP/1.1\r\nHost: example.test\r\n\r\nGET /two HTTP/1.1\r\nHost: example.test\r\n\r\n",
        2
    );
    auto second = result.find("HTTP/1.1 200", 1);
    require(
        second != std::string::npos && result.find("\r\n\r\n/one") != std::string::npos && result.ends_with("/two"),
        "HTTP pipelining must preserve request boundaries"
    );
    require(
        result.find("Connection: keep-alive") < second && result.find("Connection: close", second) != std::string::npos,
        "HTTP request-count limit must close final response"
    );

    pace::tcp::stream fragmented(context);
    co_await fragmented.connect(endpoint);
    co_await fragmented.write_all(pace::as_bytes("POST /echo HTTP/1.1\r\nHo"));
    co_await context.sleep_for(1ms);
    co_await fragmented.write_all(
        pace::as_bytes("st: example.test\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhe")
    );
    co_await context.sleep_for(1ms);
    co_await fragmented.write_all(pace::as_bytes("llo"));
    result = co_await read_responses(fragmented);
    require(result.ends_with("\r\n\r\nhello"), "fragmented HTTP head/body");

    struct invalid_case
    {
        std::string request;
        unsigned status;
    };

    const std::array cases{
        invalid_case{"GET / HTTP/1.1\r\n\r\n", 400},
        invalid_case{"GET / HTTP/1.1\r\nHost: example.test\r\nHost: other\r\n\r\n", 400},
        invalid_case{"POST / HTTP/1.1\r\nHost: example.test\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n", 400},
        invalid_case{"POST / HTTP/1.1\r\nHost: example.test\r\nContent-Length: +1\r\n\r\n", 400},
        invalid_case{"POST / HTTP/1.1\r\nHost: example.test\r\nTransfer-Encoding: gzip\r\n\r\n", 501},
        invalid_case{
            "POST / HTTP/1.1\r\nHost: example.test\r\nTransfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n",
            400
        },
        invalid_case{"GET / HTTP/1.1\r\nHost: example.test\r\n bad: fold\r\n\r\n", 400},
        invalid_case{"GET / HTTP/1.1\r\nHost : example.test\r\n\r\n", 400},
        invalid_case{"GET / HTTP/1.1\r\nHost: example.test\nbad: value\r\n\r\n", 400},
        invalid_case{"POST / HTTP/1.1\r\nHost: example.test\r\nContent-Length: 33\r\n\r\n", 413},
        invalid_case{"POST / HTTP/1.1\r\nHost: example.test\r\nExpect: 100-continue\r\n\r\n", 417},
        invalid_case{"HEAD / HTTP/1.1\r\nHost: example.test\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n", 400},
        invalid_case{"HEAD / HTTP/1.1\r\nHost: example.test\r\nExpect: 100-continue\r\n\r\n", 417},
        invalid_case{"GET / HTTP/1.1\r\nHost: example.test\r\nX-Large: " + std::string(256, 'x') + "\r\n\r\n", 431},
        invalid_case{"HEAD / HTTP/1.1\r\nHost: example.test\r\nX-Large: " + std::string(256, 'x') + "\r\n\r\n", 431}
    };
    for (const auto& test : cases)
    {
        try
        {
            result = co_await exchange(context, endpoint, test.request);
        }
        catch (const std::exception& error)
        {
            throw std::runtime_error(
                "HTTP error-response case (expected " + std::to_string(test.status) + "): " + error.what()
            );
        }
        require(result.starts_with("HTTP/1.1 " + std::to_string(test.status)), "malformed request response status");
        require(result.find("Connection: close") != std::string::npos, "malformed request must close connection");
        if (test.request.starts_with("HEAD "))
        {
            require(result.ends_with("\r\n\r\n"), "HEAD errors forbid payload bytes");
        }
    }
    result =
        co_await exchange(context, endpoint, "HEAD /head HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n");
    require(
        result.find("Content-Length: 5\r\n") != std::string::npos && result.ends_with("\r\n\r\n"),
        "HEAD response framing"
    );
    result =
        co_await exchange(context, endpoint, "GET /empty HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n");
    require(
        result.starts_with("HTTP/1.1 204") && result.find("Content-Length") == std::string::npos
            && result.ends_with("\r\n\r\n"),
        "204 forbids body and Content-Length"
    );
    result = co_await exchange(context, endpoint, "GET /legacy HTTP/1.0\r\n\r\n");
    require(
        result.ends_with("/legacy") && result.find("Connection: close") != std::string::npos,
        "HTTP/1.0 defaults to close"
    );
    result = co_await exchange(context, endpoint, "GET /bad HTTP/1.1\r\nHost: example.test\r\n\r\n");
    require(
        result.starts_with("HTTP/1.1 500") && result.find("X-Injected") == std::string::npos,
        "response header injection must be rejected"
    );
}

#endif

pace::task<void> stop_soon(pace::io_context& context)
{
    co_await context.sleep_for(5ms);
    context.stop();
}

pace::task<void> stop_pending_accept(pace::io_context& context)
{
    pace::tcp::listener listener(context, {"127.0.0.1", 0});
    context.spawn(stop_soon(context));
    bool canceled = false;
    try
    {
        auto unused = co_await listener.accept();
    }
    catch (const std::system_error& error)
    {
        canceled = error.code() == std::errc::operation_canceled;
    }
    require(canceled, "stop must cancel a suspended accept");
}

pace::task<void> accept_idle(std::shared_ptr<pace::tcp::listener> listener)
{
    auto stream = co_await listener->accept();
    std::array<char, 8> buffer;
    try
    {
        co_await stream.read_some(pace::as_writable_bytes(buffer));
    }
    catch (const std::system_error& error)
    {
        if (error.code() != std::errc::operation_canceled)
        {
            throw;
        }
    }
}

pace::task<void> stop_pending_read(pace::io_context& context)
{
    auto listener = std::make_shared<pace::tcp::listener>(context, pace::tcp::endpoint{"127.0.0.1", 0});
    context.spawn(accept_idle(listener));
    pace::tcp::stream client(context);
    co_await client.connect(listener->local_endpoint());
    context.spawn(stop_soon(context));
    bool canceled = false;
    std::array<char, 8> buffer;
    try
    {
        co_await client.read_some(pace::as_writable_bytes(buffer));
    }
    catch (const std::system_error& error)
    {
        canceled = error.code() == std::errc::operation_canceled;
    }
    require(canceled, "stop must cancel a suspended read");
}

pace::task<void> read_into(std::shared_ptr<pace::tcp::stream> stream, std::shared_ptr<std::string> received)
{
    std::array<char, 8> buffer{};
    const auto count = co_await stream->read_some(pace::as_writable_bytes(buffer));
    received->assign(buffer.data(), count);
}

pace::task<void> accept_into(std::shared_ptr<pace::tcp::listener> listener, std::shared_ptr<bool> accepted)
{
    (void)co_await listener->accept();
    *accepted = true;
}

// A second operation in the same direction is rejected up front, before it can take data or a connection that
// belongs to the suspended first one.
pace::task<void> second_operation_rejected(pace::io_context& context)
{
    auto listener = std::make_shared<pace::tcp::listener>(context, pace::tcp::endpoint{"127.0.0.1", 0});
    pace::tcp::stream client(context);
    co_await client.connect(listener->local_endpoint());
    auto server = std::make_shared<pace::tcp::stream>(co_await listener->accept());

    auto received = std::make_shared<std::string>();
    context.spawn(read_into(server, received));
    co_await context.yield(); // the first read runs until it waits for data
    co_await client.write_all(pace::as_bytes("x"));
    // Wait until the byte has reached the server (loopback delivery can be asynchronous), so that an unguarded
    // second read would find it instead of waiting.
    for (;;)
    {
        char peeked = 0;
        if (::recv(server->native_handle(), &peeked, 1, MSG_PEEK) == 1)
        {
            break;
        }
        co_await context.yield();
    }
    bool read_rejected = false;
    try
    {
        std::array<char, 8> buffer{};
        (void)co_await server->read_some(pace::as_writable_bytes(buffer));
    }
    catch (const std::logic_error&)
    {
        read_rejected = true;
    }
    require(read_rejected, "a second TCP read was not rejected");
    while (received->empty())
    {
        co_await context.yield();
    }
    require(*received == "x", "the first read lost its data");

    auto accepted = std::make_shared<bool>(false);
    context.spawn(accept_into(listener, accepted));
    co_await context.yield();
    bool accept_rejected = false;
    try
    {
        (void)co_await listener->accept();
    }
    catch (const std::logic_error&)
    {
        accept_rejected = true;
    }
    require(accept_rejected, "a second TCP accept was not rejected");
    pace::tcp::stream another(context);
    co_await another.connect(listener->local_endpoint());
    while (!*accepted)
    {
        co_await context.yield();
    }
}

pace::task<std::string> read_to_end(pace::tcp::stream& stream)
{
    std::string received;
    std::array<char, 64> bytes{};
    while (const auto count = co_await stream.read_some(pace::as_writable_bytes(bytes)))
    {
        received.append(bytes.data(), count);
    }
    co_return received;
}

pace::task<void> half_close_server(std::shared_ptr<pace::tcp::listener> listener)
{
    auto stream = co_await listener->accept();
    require(co_await read_to_end(stream) == "ping", "TCP half-close request mismatch");
    co_await stream.write_all(pace::as_bytes("pong"));
}

// shutdown_write sends FIN; the peer reads EOF and can still answer, and this side can still read the answer.
pace::task<void> tcp_half_close(pace::io_context& context)
{
    auto listener = std::make_shared<pace::tcp::listener>(context, pace::tcp::endpoint{"127.0.0.1", 0});
    context.spawn(half_close_server(listener));
    pace::tcp::stream client(context);
    co_await client.connect(listener->local_endpoint());
    co_await client.write_all(pace::as_bytes("ping"));
    co_await client.shutdown_write();
    require(co_await read_to_end(client) == "pong", "TCP reply after shutdown_write was lost");
}

pace::task<void> close_soon(std::shared_ptr<pace::tcp::stream> stream)
{
    co_await stream->context().sleep_for(5ms);
    stream->close();
}

pace::task<void> close_pending_read(pace::io_context& context)
{
    auto listener = std::make_shared<pace::tcp::listener>(context, pace::tcp::endpoint{"127.0.0.1", 0});
    context.spawn(accept_idle(listener));
    auto client = std::make_shared<pace::tcp::stream>(context);
    co_await client->connect(listener->local_endpoint());
    context.spawn(close_soon(client));
    bool canceled = false;
    std::array<char, 8> buffer;
    try
    {
        co_await client->read_some(pace::as_writable_bytes(buffer));
    }
    catch (const std::system_error& error)
    {
        canceled = error.code() == std::errc::operation_canceled;
    }
    require(canceled && !context.stopped(), "socket close must cancel only its own suspended read");
}

struct graceful_close_result
{
    bool finished = false;
    std::chrono::steady_clock::duration elapsed{};
};

pace::task<void> close_after_reply(
    std::shared_ptr<pace::tcp::listener> listener,
    std::shared_ptr<graceful_close_result> result
)
{
    auto stream = co_await listener->accept();
    // Wait for the peer's input before closing. Reading one byte proves the
    // whole single-segment write is queued here, so the rest is unread input
    // at close time; closing before it arrives would let it reset the peer.
    std::array<char, 1> first{};
    require(co_await stream.read_some(pace::as_writable_bytes(first)) == 1, "graceful close peer input missing");
    co_await stream.write_all(pace::as_bytes("done"));
    const auto started = std::chrono::steady_clock::now();
    co_await stream.close_gracefully(5ms);
    result->elapsed = std::chrono::steady_clock::now() - started;
    result->finished = true;
    require(!stream.is_open(), "graceful close must release its socket");
}

pace::task<void> graceful_close_bounds(pace::io_context& context)
{
    auto listener = std::make_shared<pace::tcp::listener>(context, pace::tcp::endpoint{"127.0.0.1", 0});
    auto result = std::make_shared<graceful_close_result>();
    context.spawn(close_after_reply(listener, result));
    pace::tcp::stream client(context);
    co_await client.connect(listener->local_endpoint());
    co_await client.write_all(pace::as_bytes("discard this pending input"));
    std::array<char, 4> reply{};
    std::size_t received = 0;
    while (received < reply.size())
    {
        const auto count = co_await client.read_some(pace::as_writable_bytes(std::span<char>(reply).subspan(received)));
        require(count != 0, "graceful close truncated its final reply");
        received += count;
    }
    require(std::string_view(reply.data(), reply.size()) == "done", "graceful close reply mismatch");
    // Keep our send side open: the server must finish when its drain timeout
    // expires instead of waiting indefinitely for peer EOF. Poll for that
    // rather than sleeping a fixed time, which a loaded machine can overrun;
    // the test watchdog bounds a server that never finishes.
    while (!result->finished)
    {
        co_await context.sleep_for(1ms);
    }
    require(result->elapsed >= 5ms, "graceful close must honor its drain deadline");
    require(
        co_await client.read_some(pace::as_writable_bytes(reply)) == 0,
        "drained connection must close without reset"
    );
}

pace::task<void> graceful_close_timer_cleanup(pace::io_context& context)
{
    pace::tcp::listener listener(context, {"::1", 0});
    {
        pace::tcp::stream client(context);
        co_await client.connect(listener.local_endpoint());
        auto accepted = co_await listener.accept();
        client.close();
        co_await accepted.close_gracefully(10ms);
        require(!accepted.is_open(), "peer EOF must finish graceful close");
    }
    // The deadline captured the destroyed stream and coroutine frame. It must
    // already have been removed, including when EOF completed synchronously.
    co_await context.sleep_for(25ms);
    co_await endpoint_exchange(context, listener, listener.local_endpoint());
}

pace::task<void> hold_connection(std::shared_ptr<pace::tcp::listener> listener)
{
    auto stream = co_await listener->accept();
    try
    {
        co_await stream.context().sleep_for(24h);
    }
    catch (const std::system_error& error)
    {
        if (error.code() != std::errc::operation_canceled)
        {
            throw;
        }
    }
}

pace::task<void> graceful_close_cancellation(pace::io_context& context)
{
    auto listener = std::make_shared<pace::tcp::listener>(context, pace::tcp::endpoint{"127.0.0.1", 0});
    context.spawn(hold_connection(listener));
    pace::tcp::stream client(context);
    co_await client.connect(listener->local_endpoint());
    context.spawn(stop_soon(context));
    bool canceled = false;
    try
    {
        co_await client.close_gracefully(1s);
    }
    catch (const std::system_error& error)
    {
        canceled = error.code() == std::errc::operation_canceled;
    }
    require(canceled && !client.is_open(), "canceled graceful close must release its socket");
}

pace::task<void> exact_reads(pace::io_context& io)
{
    auto listener = pace::tcp::listen({"127.0.0.1", 0});
    auto client = co_await pace::tcp::connect(listener.local_endpoint());
    auto peer = co_await listener.accept();
    auto owned_write = pace::write(peer, std::string("ab\0cdef", 7));
    co_await io.yield();
    co_await std::move(owned_write);
    require((co_await pace::read_exact(client, 3)) == std::string("ab\0", 3), "owned exact binary read");
    std::array<std::byte, 2> pair{};
    co_await pace::read_exact(client, std::span<std::byte>(pair));
    require(pair[0] == std::byte{'c'} && pair[1] == std::byte{'d'}, "exact byte span overconsumed");
    bool bounded = false;
    try
    {
        (void)co_await pace::read_exact(client, 1025, 1024);
    }
    catch (const std::length_error&)
    {
        bounded = true;
    }
    require(bounded && (co_await client.read_exact(2)) == "ef", "bounded read consumed bytes");
    require((co_await client.read_exact(0)).empty(), "empty member exact read blocked");
    require((co_await pace::read_exact(client, 0)).empty(), "empty shared exact read blocked");
    co_await peer.write_all(pace::as_bytes("x"));
    peer.close();
    bool eof = false;
    try
    {
        (void)co_await pace::read_exact(client, 2);
    }
    catch (const std::system_error& error)
    {
        eof = error.code() == std::errc::connection_reset;
    }
    require(eof, "exact read silently returned a truncated message");
    auto pending = co_await pace::tcp::connect(listener.local_endpoint(), {.context = &io});
    auto waiting_peer = co_await listener.accept();
    io.spawn(stop_soon(io));
    bool canceled = false;
    try
    {
        (void)co_await pace::read_exact(pending, 1);
    }
    catch (const std::system_error& error)
    {
        canceled = error.code() == std::errc::operation_canceled;
    }
    require(canceled, "exact read lost cancellation");
}

static_assert(std::same_as<decltype(std::declval<pace::tcp::stream&>().connect({}, {})), pace::task<void>>);

pace::task<void> tcp_results(pace::io_context& io)
{
    using pace::as_result;
    auto listener = pace::tcp::listen(io, {"127.0.0.1", 0});
    const auto address = listener.local_endpoint();
    auto connected = co_await pace::tcp::connect(as_result, address);
    require(connected && !connected.error(), "TCP result connect");
    auto client = std::move(connected).value();
    auto accepted = co_await listener.accept(as_result);
    require(bool(accepted), "TCP result accept");
    auto peer = std::move(accepted).value();

    std::array<std::byte, 1> byte{};
    // A deadline while suspended reports its own reason and releases the pending-read guard.
    auto timed = co_await pace::with_deadline(pace::deadline::after(5ms), client.read_some(as_result, byte));
    require(!timed && timed.error() == std::errc::timed_out, "TCP read deadline result");

    co_await pace::with_task_group(
        [&](pace::task_group& group) -> pace::task<void>
        {
            auto waiting = group.spawn(client.read_some(as_result, byte));
            co_await io.yield();
            auto duplicate = co_await client.read_some(as_result, byte);
            require(!duplicate && duplicate.error() == std::errc::operation_in_progress, "overlapping read result");
            auto wake = co_await peer.write_all(as_result, pace::as_bytes("w"));
            auto received = co_await waiting;
            require(
                wake && received && received.value() == 1 && byte[0] == std::byte{'w'},
                "original read survived overlap"
            );
        }
    );

    pace::cancellation_source canceled;
    auto skipped = co_await pace::with_cancellation(
        canceled.token(),
        [&]
        {
            canceled.request_cancellation();
            return peer.write_all(as_result, pace::as_bytes("x"));
        }
    );
    require(!skipped && skipped.error() == std::errc::operation_canceled, "pre-canceled write result");
    auto sent = co_await peer.write_all(as_result, pace::as_bytes("y"));
    auto read = co_await client.read_some(as_result, byte);
    require(sent && read && read.value() == 1 && byte[0] == std::byte{'y'}, "TCP result bytes");
    require(bool(co_await as_result(peer.shutdown_write())), "void task result");
    auto eof = co_await client.read_some(as_result, byte);
    require(eof && eof.value() == 0 && !eof.error(), "TCP EOF is a successful zero");

    client.close();
    auto bad_read = co_await client.read_some(as_result, byte);
    auto bad_write = co_await client.write_all(as_result, {});
    require(!bad_read && bad_read.error() == std::errc::bad_file_descriptor, "closed TCP read result");
    require(!bad_write && bad_write.error() == std::errc::bad_file_descriptor, "closed TCP write result");

    auto close_listener = io.call_after(5ms, [&] { listener.close(); });
    auto interrupted = co_await listener.accept(as_result);
    require(!interrupted && interrupted.error() == std::errc::operation_canceled, "close during accept result");
    // No accept was registered on this listener, so a pending kernel poll cannot retain it after close.
    auto unused = pace::tcp::listen(io, {"127.0.0.1", 0});
    const auto unavailable = unused.local_endpoint();
    unused.close();
    auto refused = co_await pace::tcp::connect(as_result, unavailable, {.context = &io});
    require(
        !refused && refused.error() == std::errc::connection_refused,
        "TCP connect refusal result: " + refused.error().message()
    );
    auto adapted = co_await as_result(pace::tcp::connect(unavailable, {.context = &io}));
    require(!adapted && adapted.error() == refused.error(), "adapted move-only task preserves its socket error");
    pace::tcp::stream fresh(io);
    auto invalid = co_await fresh.connect(as_result, {"invalid", 42});
    require(!invalid && invalid.error() == std::errc::invalid_argument, "invalid TCP address result");
}

using test_body = pace::task<void> (*)(pace::io_context&);

pace::task<void> run_body(pace::io_context& context, test_body body, bool& completed)
{
    co_await body(context);
    completed = true;
    context.stop();
}

void run_test(test_body body)
{
    pace::io_context context;
    bool completed = false;
    std::mutex mutex;
    std::condition_variable condition;
    bool finished = false;
    std::atomic<bool> timed_out = false;
    std::thread watchdog(
        [&]
        {
            std::unique_lock lock(mutex);
            if (!condition.wait_for(lock, 10s, [&] { return finished; }))
            {
                timed_out = true;
                context.stop();
            }
        }
    );
    auto finish = [&]
    {
        {
            std::lock_guard lock(mutex);
            finished = true;
        }
        condition.notify_one();
        watchdog.join();
    };
    try
    {
        context.spawn(run_body(context, body, completed));
        context.run();
    }
    catch (...)
    {
        finish();
        throw;
    }
    finish();
    require(!timed_out && completed, "network test timed out or did not complete");
}
} // namespace

#ifndef PACE_CORE_ONLY_TESTS
TEST_CASE("HTTP response framing helper accepts only complete frames")
{
    verify_response_framing();
}
#endif

TEST_CASE("TCP binary roundtrip and ownership")
{
    run_test(tcp_roundtrip);
}

TEST_CASE("queued accepts yield to the reactor")
{
    run_test(accept_burst);
}

TEST_CASE("large TCP write preserves payload across yields")
{
    run_test(large_write);
}

TEST_CASE("easy connect, bounded exact reads, EOF and cancellation")
{
    run_test(exact_reads);
}

TEST_CASE("connect and endpoint errors")
{
    run_test(connect_errors);
}

TEST_CASE("IPv6 loopback, endpoint inspection and family selection")
{
    run_test(ipv6_loopback);
}

TEST_CASE("IPv6 wildcard, IPv6-only and dual-stack listeners")
{
    run_test(ipv6_wildcards);
}

TEST_CASE("IPv6 family, scope and malformed endpoint validation")
{
    run_test(ipv6_address_validation);
}

TEST_CASE("short-read rearm, queued bytes, move, empty buffer and EOF")
{
    run_test(short_read_rearm);
}

#ifndef PACE_CORE_ONLY_TESTS
TEST_CASE("HTTP framing, fragmentation, limits and keep-alive")
{
    run_test(http_protocol);
}
#endif

TEST_CASE("a second read or accept is rejected while the first is pending")
{
    run_test(second_operation_rejected);
}

TEST_CASE("TCP shutdown_write sends FIN and keeps reading")
{
    run_test(tcp_half_close);
}

TEST_CASE("stop cancels pending accept")
{
    run_test(stop_pending_accept);
}

TEST_CASE("stop cancels pending read")
{
    run_test(stop_pending_read);
}

TEST_CASE("socket close cancels pending read")
{
    run_test(close_pending_read);
}

TEST_CASE("graceful close drains input and bounds peer wait")
{
    run_test(graceful_close_bounds);
}

TEST_CASE("completed IPv6 graceful close removes its deadline")
{
    run_test(graceful_close_timer_cleanup);
}

TEST_CASE("stop cancels graceful close")
{
    run_test(graceful_close_cancellation);
}

TEST_CASE("native TCP and UDP adoption and pre-connect socket configuration")
{
    run_test(adopt_and_configure);
}

TEST_CASE("pre-connect hook can abort without initiating a connection")
{
    run_test(connect_hook_failure);
}

#if defined(TCP_FASTOPEN)
TEST_CASE("TCP Fast Open listener and exchange preserve the first write")
{
    run_test(fast_open_exchange);
}
#endif

TEST_CASE("TCP listener and client apply native socket options")
{
    run_test(verify_tcp_options);
    run_test(verify_routing_options);
}

#if defined(__APPLE__) || defined(__FreeBSD__)
// tests/net/transparent_pf.sh supplies loopback redirection and access to /dev/pf.
TEST_CASE("PF original destination after redirection" * doctest::skip(std::getenv("PACE_TEST_PF_REDIRECT") == nullptr))
{
    run_test(original_destinations);
}
#elif defined(__linux__)
TEST_CASE("netfilter original destinations" * doctest::skip(std::getenv("PACE_TEST_TRANSPARENT") == nullptr))
{
    run_test(original_destinations);
}
#endif

TEST_CASE("relay forwards both half-closes and accounts for both directions")
{
    run_test(relay_exchange);
}

TEST_CASE("relay closes both streams on idle or half-close timeout")
{
    run_test(relay_timeouts);
}

TEST_CASE("relay copy and optional splice deliver identical multi-buffer payloads")
{
    run_test(relay_payloads);
}

TEST_CASE("cancelling an active relay closes both streams")
{
    run_test(relay_cancel);
}

TEST_CASE("relay obeys the observer's cumulative write budget")
{
    run_test(relay_pacing);
}

TEST_CASE("TCP result overloads report success, EOF, socket errors and cancellation")
{
    run_test(tcp_results);
}
