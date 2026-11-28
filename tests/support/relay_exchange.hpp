#pragma once

#include <pace/net/relay.hpp>
#include <pace/runtime/run.hpp>
#include "support/pace_test.hpp"

namespace pace::test
{
// Exercise a real four-ended relay. Both directions carry bytes before the first FIN,
// and the responder sends a multi-record binary reply only after observing that FIN.
template <byte_stream Left, byte_stream Right, byte_stream Client, byte_stream Server>
task<void> relay_exchange(Left& left, Right& right, Client& client, Server& server)
{
    using namespace std::chrono_literals;
    std::string request(96 * 1024 + 13, '\0');
    for (std::size_t i = 0; i < request.size(); ++i)
    {
        request[i] = static_cast<char>(i % 251);
    }
    const std::string reply(request.rbegin(), request.rend());
    co_await with_task_group(
        [&](task_group& group) -> task<void>
        {
            auto pumping = group.spawn(relay(left, right, {.idle_timeout = 5s, .linger = 5s}));
            auto responder = group.spawn(
                [&]() -> task<void>
                {
                    require((co_await read_exact(server, 4)) == "ping", "relay request header");
                    co_await server.write_all(as_bytes(reply));
                    require((co_await read_exact(server, request.size())) == request, "relay binary request");
                    std::array<std::byte, 1> byte{};
                    require((co_await server.read_some(byte)) == 0, "relay request EOF");
                    co_await server.write_all(as_bytes(reply));
                    co_await server.shutdown_write();
                }
            );
            co_await write(client, "ping");
            co_await client.write_all(as_bytes(request));
            require((co_await read_exact(client, reply.size())) == reply, "relay response before request FIN");
            co_await client.shutdown_write();
            require((co_await read_exact(client, reply.size())) == reply, "relay binary reply after FIN");
            std::array<std::byte, 1> byte{};
            require((co_await client.read_some(byte)) == 0, "relay reply EOF");
            co_await responder;
            const auto result = co_await pumping;
            require(result.a_to_b == request.size() + 4 && result.b_to_a == 2 * reply.size(), "relay byte counts");
        }
    );
}
} // namespace pace::test
