# 6. UDP

[中文](06-udp.zh.md) · [Learning path](README.md) · [Previous: TCP](05-tcp.md)

UDP keeps datagram boundaries but promises neither delivery nor order. This chapter builds a UDP client and an echo
server; both link `pace_net`.

## Write a direct client

Save this as `udp_client.cpp`:

```cpp
#include <pace/net.hpp>
#include <iostream>

int main()
{
    try
    {
        auto packet = pace::udp::exchange({"127.0.0.1", 9091}, "Hello, UDP!");
        if (packet.truncated)
        {
            throw std::runtime_error("UDP reply truncated");
        }
        std::cout << packet.data << '\n';
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`exchange` sends one datagram and returns the first reply that comes from that same address and port; datagrams
from other senders are ignored. It blocks this thread, with a total budget of 30 seconds by default, and needs no
event-loop setup. For two seconds, include `<chrono>` and pass `{.timeout = std::chrono::seconds(2)}` as the third
argument.

The result owns `data` and reports `sender` and `truncated`. Check `truncated` before using the message: the bytes
beyond the receive capacity (64 KiB by default) are lost. An empty `data` is a valid zero-length datagram, not an end
of stream. Nothing is retried automatically.

## Supply a UDP echo service

Save this as `udp_server.cpp`:

```cpp
#include <pace/net.hpp>
#include <iostream>

pace::task<void> serve()
{
    auto socket = pace::udp::bind({"127.0.0.1", 9091});
    for (;;)
    {
        auto packet = co_await socket.receive();
        if (!packet.truncated)
        {
            co_await socket.send(std::move(packet.data), packet.sender);
        }
    }
}

int main()
{
    try
    {
        pace::run(serve);
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`bind` creates a socket on the running context. `receive` waits for one datagram, and `send` replies to its sender.
This server drops truncated datagrams. The socket lives in the coroutine frame for as long as the loop runs, and
`run` drives the server until you stop it.

## Build and try it

Add these targets to `xmake.lua`:

```lua
for _, name in ipairs({"udp_client", "udp_server"}) do
    target(name)
        set_kind("binary")
        add_files(name .. ".cpp")
        add_deps("pace_net")
end
```

Run `xmake -y udp_client udp_server`, then `xmake run udp_server`. In another terminal run `xmake run udp_client`;
expect `Hello, UDP!`. Try another string. Stop the server with Ctrl-C, then run the client again: depending on how
the operating system reports an unreachable port, you see a connection error or, after the budget, a timeout.

## Send only, or compose async work

For a service that sends no reply:

```cpp
auto sent = pace::udp::send({"127.0.0.1", 9091}, "notification");
```

The result is the number of bytes the local socket accepted, not a delivery receipt. Each direct call uses a fresh
socket. Inside a coroutine, the async request is:

```cpp
auto packet = co_await pace::udp::async_exchange({"127.0.0.1", 9091}, "hello");
```

The async form adds no total budget. To reuse a socket or accept replies from other addresses, use `udp::open()` or
`udp::bind()` with `socket.send` and `socket.receive` in your coroutine, as the server does.

The companion `xmake run tutorial_udp` ([source](../../examples/tutorial_udp.cpp)) binds a receiver, sends a
datagram to it from a second socket opened with `udp::open()`, checks it and prints `Hello, UDP!`.

[UDP reference](../reference/net/udp.md) · [Protocol tuning](../guides/protocol-tuning.md)

Next: [7. Structured concurrency](07-structured-concurrency.md)
