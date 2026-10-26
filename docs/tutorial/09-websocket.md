# 9. Upgrade a connection and exchange WebSocket messages

[中文](09-websocket.zh-CN.md) · [Previous: DNS/TLS](08-dns-tls.md) · [Next: HTTP/2](10-http2.md)

We will reuse chapter 3's accept loop, but replace the per-connection byte echo
with a WebSocket upgrade and message echo. Enable `--websocket=y`, link
`pace_websocket`, and include `<pace/websocket.hpp>`.

## 1. Accept the upgrade before receiving messages

Inside the server's connection task, take ownership of the accepted TCP stream:

```cpp
pace::websocket::stream socket(std::move(transport), pace::websocket::role::server);
co_await socket.accept({"echo"}, [](const auto& request) {
    return request.target == "/echo";
});
while (auto message = co_await socket.receive())
    co_await socket.send(std::move(*message));
```

The role selects server handshake/framing behavior. The callback checks the
upgrade target before admitting the connection; a real application can also
check Origin and authentication. `receive` returns a whole reassembled message,
not an individual TCP read or continuation frame. An empty optional signals close;
an empty message body is still a message.

Use the TCP chapter's per-connection error boundary, additionally catching
`websocket::protocol_error` for rejected/malformed upgrades and frames. Keep the
accept loop available to other clients.

## 2. Connect, negotiate and verify one message

In the client task, after awaiting TCP connect, write:

```cpp
pace::websocket::stream socket(std::move(transport), pace::websocket::role::client);
co_await socket.handshake("localhost", "/echo", {"echo"});
co_await socket.send_text("Hello from PACE", 4);
const auto reply = co_await socket.receive();
if (!reply || reply->data != "Hello from PACE")
    throw std::runtime_error("WebSocket echo mismatch");
const bool closed = co_await socket.close_gracefully(1000, "finished");
```

The optional size 4 fragments the outgoing message; receive reconstructs it.
`closed` reports whether the closing handshake completed within its bounds.
Close before stopping the context. TCP EOF and the WebSocket close handshake are
not interchangeable. This API permits **one active asynchronous operation per
WebSocket stream**, including send/receive/control/close; do not start independent
reader and writer tasks on it as if it had TCP's one-read-plus-one-write contract.

## 3. Build and observe the complete connection lifecycle

The [companion](../../examples/tutorial_websocket.cpp) assembles these functions
with the chapter 3 listener/control structure. In the repository:

```sh
xmake f --websocket=y --examples=y -y
xmake -y tutorial_websocket
xmake run tutorial_websocket
```

Expect `WebSocket echo verified: Hello from PACE`. To try separate processes, run
`xmake run tutorial_websocket serve` and then `xmake run tutorial_websocket client`
in another terminal. Change the admitted path in your copy and confirm an
incorrect upgrade is rejected. Then restore it and try a larger fragmented text.
TLS-backed WebSocket uses the secure stream types with the verified TLS setup from
chapter 8; see [the reference](../reference/websocket.md) for limits and exact types.
