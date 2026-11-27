# 11. WebSocket

[中文](11-websocket.zh.md) · [Learning path](README.md) · [Previous: TLS and DNS](10-tls-and-dns.md)

WebSocket is enabled by default (`--websocket=y`); link `pace_websocket`. Set up two targets, `ws_server` and
`ws_client`, the same way as the TCP pair in [chapter 5](05-tcp.md). This exercise accepts **one connection**, then
both programs exit.

## Accept an upgrade

Save this as `ws_server.cpp`:

```cpp
#include <pace/runtime.hpp>
#include <pace/websocket.hpp>
#include <iostream>

pace::task<void> serve()
{
    auto listener = pace::tcp::listen({"127.0.0.1", 9092});
    std::vector<std::string> protocols{"echo"};
    auto socket = co_await pace::websocket::accept(
        co_await listener.accept(),
        std::move(protocols),
        [](const auto& request) { return request.target == "/echo"; }
    );
    while (auto message = co_await socket.receive())
    {
        co_await socket.send(std::move(*message));
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

`websocket::accept` takes ownership of the TCP stream and completes the server side of the handshake. The callback
admits only the `/echo` path; put your application's `Origin` or identity checks there too. `receive` returns a whole
message, assembled from its frames. An empty optional means the peer's close frame arrived; an empty message is
still a message. The loop echoes until the client closes.

## Connect and send

Save this as `ws_client.cpp`:

```cpp
#include <pace/runtime.hpp>
#include <pace/websocket.hpp>
#include <iostream>

pace::task<void> exchange()
{
    std::vector<std::string> protocols{"echo"};
    auto socket =
        co_await pace::websocket::connect({"127.0.0.1", 9092}, "localhost:9092", "/echo", std::move(protocols));
    co_await socket.send_text("Hello from PACE");
    const auto reply = co_await socket.receive();
    if (!reply || reply->data != "Hello from PACE")
    {
        throw std::runtime_error("WebSocket echo mismatch");
    }
    std::cout << reply->data << '\n';
    if (!co_await socket.close_gracefully())
    {
        throw std::runtime_error("WebSocket close timed out");
    }
}

int main()
{
    try
    {
        pace::run(exchange);
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`connect` opens the TCP connection and performs the WebSocket handshake. The numeric endpoint says where to
connect; `localhost:9092` is the HTTP `Host` authority. Both peers offer the `echo` subprotocol. Await the send, then
the receive, then the close: a WebSocket stream allows only **one asynchronous operation at a time**.
`close_gracefully` returns `false` if the peer does not answer the close within its budget (100 ms by default).

## Run and change one behavior

Build both targets. Run `xmake run ws_server` first, then `xmake run ws_client` in another terminal. Expect
`Hello from PACE`; both programs exit after the closing handshake. Start the server again for each client. Change
the client's path to `/wrong` and check that the server rejects the upgrade.

The companion [`tutorial_websocket.cpp`](../../examples/tutorial_websocket.cpp) runs both peers in one process. Its
client also sends a ping and sends the text in 4-byte fragments that `receive` reassembles, and its server accepts
connections in a loop. `xmake run tutorial_websocket` prints `WebSocket echo verified: Hello from PACE`;
`xmake run tutorial_websocket serve` keeps the server on port 9092 until Ctrl-C, and
`xmake run tutorial_websocket client` connects to it.

To encrypt the connection, open a verified TLS stream with `tls::connect` from [chapter 10](10-tls-and-dns.md) and
pass it to `websocket::upgrade`; on the server, `websocket::accept` also takes a TLS stream.
[WebSocket reference](../reference/websocket/README.md) · [Protocol tuning](../guides/protocol-tuning.md) for
fragments, limits and close budgets.

Next: [12. HTTP/2](12-http2.md)
