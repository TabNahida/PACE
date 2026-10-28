# 7. Send and receive a UDP message

[中文](07-udp.zh-CN.md) · [Previous: shutdown](06-shutdown.md) · [Next: DNS/TLS](08-dns-tls.md)

Use chapter 1's current checkout and `pace_net`. Unlike TCP, UDP preserves a
send's message boundary, but does not promise delivery or ordering. Replace
`main.cpp` with this complete local exercise:

```cpp
#include <pace/udp.hpp>
#include <pace/run.hpp>
#include <iostream>

pace::task<void> hello() {
    auto receiver = pace::udp::bind({"127.0.0.1", 0});
    auto sender = pace::udp::open();
    co_await sender.send("Hello, UDP!", receiver.local_endpoint());
    auto packet = co_await receiver.receive();
    if (packet.truncated || packet.data != "Hello, UDP!")
        throw std::runtime_error("UDP message mismatch or truncation");
    std::cout << packet.data << '\n';
}
int main() {
    try { pace::run(hello); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
```

## 1. Give the receiver an address

`udp::bind` returns a bound socket. Port zero asks the OS to choose a free port;
`local_endpoint()` reports it. `udp::open()` creates an unbound sender whose first
send selects its local address. Neither helper needs an explicit context inside
`pace::run`.

## 2. Read the message itself

`send` owns the string across suspension. `receive` returns `packet.data`,
`packet.sender` and `packet.truncated`; no buffer or byte-count bookkeeping is
needed. Check truncation before interpreting the data: an oversized datagram loses
its remaining bytes. An empty `data` is a valid zero-length datagram, not EOF.

## 3. Turn it into an echo

Build/run `my_application`; expect `Hello, UDP!` and process exit. Then insert this
after printing the message:

```cpp
co_await receiver.send(packet.data, packet.sender);
auto reply = co_await sender.receive();
if (reply.truncated || reply.data != "Hello, UDP!")
    throw std::runtime_error("UDP reply mismatch");
```

Run again. The sender now receives a reply addressed using the original packet's
source. Both sockets remain alive until their awaits finish. A remote service
needs an application policy for lost messages; this local exercise adds no retries.

The repository's `xmake run tutorial_udp` runs the first exercise.
[Capacity, cancellation and ownership](../reference/component-conveniences.md#udp) ·
[Reusable buffers and tuning](../reference/protocol-tuning.md#tcp-and-udp)
