# 7. Exchange a UDP packet with one call

[中文](07-udp.zh-CN.md) · [Learning path](README.md) · [Next: DNS/TLS](08-dns-tls.md)

Use PACE 0.4.0 or newer with chapter 1's source setup and link `pace_net`. UDP preserves datagram boundaries,
but does not promise delivery or ordering.

## 1. Write a direct client

Save this as `udp_client.cpp`:

```cpp
#include <pace/udp.hpp>
#include <iostream>

int main() {
    try {
        auto packet = pace::udp::exchange({"127.0.0.1", 9091}, "Hello, UDP!");
        if (packet.truncated) throw std::runtime_error("UDP reply truncated");
        std::cout << packet.data << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`exchange` sends one request and returns the first reply from that numeric endpoint.
It blocks this thread and defaults to a 30-second total timeout. No event-loop setup
is needed. For two seconds, include `<chrono>` and pass
`{.timeout = std::chrono::seconds(2)}` as the third argument.

The result owns `data` and reports `sender` and `truncated`. Check truncation before
interpreting the message: bytes beyond the receive capacity are lost. An empty
`data` is a valid zero-length datagram, not EOF. There are no automatic retries.

## 2. Supply a UDP echo service

Save this as `udp_server.cpp`:

```cpp
#include <pace/udp.hpp>
#include <iostream>

pace::task<void> serve() {
    auto socket = pace::udp::bind({"127.0.0.1", 9091});
    for (;;) {
        auto packet = co_await socket.receive();
        if (!packet.truncated)
            co_await socket.send(std::move(packet.data), packet.sender);
    }
}
int main() {
    try { pace::run(serve); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`bind` creates a socket on the current context. `receive` waits for one packet;
`send` replies to its source. This server discards incomplete packets. The socket
stays alive while its operations run. `run` drives the long-running server.

## 3. Build and try it

Add these targets to chapter 1's xmake.lua:

```lua
for _, name in ipairs({"udp_client", "udp_server"}) do
    target(name)
        set_kind("binary")
        add_files(name .. ".cpp")
        add_deps("pace_net")
end
```

Run `xmake -y udp_client udp_server`, then `xmake run udp_server`. In another
terminal run `xmake run udp_client`; expect `Hello, UDP!`. Try another string.
Stop the server with Ctrl-C. Stop it before running the client to observe an error
or timeout instead of a reply, depending on how the OS reports an unreachable peer.

## 4. Send only, or compose async work

For a service that expects no reply:

```cpp
auto sent = pace::udp::send({"127.0.0.1", 9091}, "notification");
```

This returns the byte count accepted by the local socket, not a delivery receipt.
Each direct call owns a fresh socket. For an async request inside a coroutine:

```cpp
auto packet = co_await pace::udp::async_exchange({"127.0.0.1", 9091}, "hello");
```

The async form has no added total deadline. To reuse a socket or accept replies
from other addresses, use `udp::open()` / `udp::bind()`, `socket.send` and
`socket.receive` inside your coroutine, as the server does.

[Filtering, deadlines and errors](../reference/direct-clients.md) ·
[Capacity and ownership](../reference/component-conveniences.md#udp) ·
[Reusable buffers and tuning](../reference/protocol-tuning.md#tcp-and-udp)

The repository's `xmake run tutorial_udp` runs an independent local async exercise.
