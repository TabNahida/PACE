# 7. Send and receive a UDP datagram

[中文](07-udp.zh-CN.md) · [Previous: shutdown](06-shutdown.md) · [Next: DNS and TLS](08-dns-tls.md)

TCP required accumulating bytes until a message was complete. UDP gives each send
its own datagram boundary, without TCP's reliability or ordering. This chapter
sends one loopback datagram and prints it. Link `pace_net` and write:

```cpp
#include <pace/udp.hpp>
#include <pace/run.hpp>
#include <array>
#include <iostream>

pace::task<void> hello(pace::io_context& io) {
    pace::udp::socket receiver(io, {"127.0.0.1", 0});
    pace::udp::socket sender(io);
    co_await sender.send_to("Hello, UDP!", receiver.local_endpoint());
    std::array<char, 256> buffer{};
    auto packet = co_await receiver.receive_from(buffer);
    if (packet.truncated || std::string_view(buffer.data(), packet.size) != "Hello, UDP!")
        throw std::runtime_error("UDP message mismatch or truncation");
    std::cout << std::string_view(buffer.data(), packet.size) << '\n';
}
int main() {
    try { pace::run(hello); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
```

## 1. Bind the receiving socket

`receiver(io, {"127.0.0.1", 0})` asks the OS for an available port. Read the assigned
endpoint with `local_endpoint()`. The sender can start unbound; its send path
selects a local address/port. Both sockets are local to the entry and have no
outstanding member calls when that entry returns.

## 2. Send one message and retain its boundary

`send_to` takes the destination for this datagram. `receive_from` fills your buffer
and returns its size and source endpoint, so a reply can use `packet.sender`.
Unlike the TCP loop, a zero-length UDP message is valid; do not treat it as EOF.
Choose a buffer that fits your protocol's maximum message. If `packet.truncated` is true, the buffer was too small: the remaining bytes
of that datagram were discarded. Reject the incomplete message instead of
treating the copied prefix as a complete payload.

## 3. Verify before adding a second peer

Build/run `my_application`; expect `Hello, UDP!` and process exit. The repository
companion is `xmake run tutorial_udp`. Next, change the message and verify the exact
returned size, or reply to `packet.sender` before awaiting a receive on the sender.
Keep the send buffer alive until `send_to` completes and do not overlap two sends
or two receives on one socket. UDP delivery, order, retries and message identity
are responsibilities of the application protocol. See [UDP reference](../reference/udp.md).
