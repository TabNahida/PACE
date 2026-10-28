# 9. Connect and exchange WebSocket messages

[中文](09-websocket.zh-CN.md) · [Previous: DNS/TLS](08-dns-tls.md) · [Next: HTTP/2](10-http2.md)

With the current checkout, enable `--websocket=y` and link `pace_websocket`.
Follow chapter 3's two-target layout, naming the files/targets `ws_server` and
`ws_client`. This exercise accepts **one connection**, then both processes exit.

## 1. Accept an upgrade

Write `ws_server.cpp`:

```cpp
#include <pace/websocket.hpp>
#include <pace/run.hpp>
#include <iostream>

pace::task<void> serve() {
    auto listener = pace::tcp::listen({"127.0.0.1", 9092});
    std::vector<std::string> protocols{"echo"};
    auto socket = co_await pace::websocket::accept(
        co_await listener.accept(), std::move(protocols), [](const auto& request) {
            return request.target == "/echo";
        });
    while (auto message = co_await socket.receive())
        co_await socket.send(std::move(*message));
}
int main() {
    try { pace::run(serve); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`websocket::accept` takes ownership of the TCP stream and completes the server
handshake. The callback admits `/echo`; add your application's Origin or identity
checks there. `receive` returns a whole message after assembling its frames.
An empty optional means the closing handshake was received; an empty message is
still a message. The loop echoes until the client closes.

## 2. Connect and send

Write `ws_client.cpp`:

```cpp
#include <pace/websocket.hpp>
#include <pace/run.hpp>
#include <iostream>

pace::task<void> exchange() {
    std::vector<std::string> protocols{"echo"};
    auto socket = co_await pace::websocket::connect(
        {"127.0.0.1", 9092}, "localhost:9092", "/echo", std::move(protocols));
    co_await socket.send_text("Hello from PACE");
    const auto reply = co_await socket.receive();
    if (!reply || reply->data != "Hello from PACE")
        throw std::runtime_error("WebSocket echo mismatch");
    std::cout << reply->data << '\n';
    if (!co_await socket.close_gracefully())
        throw std::runtime_error("WebSocket close timed out");
}
int main() {
    try { pace::run(exchange); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`connect` performs TCP connection and the WebSocket handshake together. The
numeric endpoint says where to connect; `localhost:9092` is the HTTP Host authority.
Both peers offer the `echo` subprotocol. Await send, then receive, then close:
this WebSocket API allows only **one asynchronous operation at a time** per stream.

## 3. Run and change one behavior

Build both targets. Run `xmake run ws_server` first, then `xmake run ws_client` in
another terminal. Expect `Hello from PACE`; both exit after the close handshake.
Restart the server for another client. Change the client's path to `/wrong` and
verify the server rejects its upgrade. For repeated clients and cooperative signal
shutdown, see `xmake run tutorial_websocket serve` and its [source](../../examples/tutorial_websocket.cpp).

To add encryption, replace TCP connect with verified `tls::connect` from chapter 8
and pass the result to `websocket::upgrade`; server-side `accept` also accepts TLS.
[Exact signatures](../reference/component-conveniences.md#websocket) ·
[Fragments, limits and close budgets](../reference/protocol-tuning.md#websocket)
