<!-- source-hash: 696905539b16 -->
# 11. WebSocket

[English](11-websocket.md) · [学习路线](README.zh.md) · [上一章：TLS 与 DNS](10-tls-and-dns.zh.md)

WebSocket 默认启用（`--websocket=y`）；链接 `pace_websocket`。按照[第 5 章](05-tcp.zh.md) TCP 那对程序的方式，建立 `ws_server` 和 `ws_client` 两个目标。这个练习只接受**一个连接**，之后两个程序都退出。

## 接受升级

保存为 `ws_server.cpp`：

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

`websocket::accept` 接管 TCP 流，并完成服务端的握手。回调只放行 `/echo` 路径；应用的 `Origin` 或身份检查也放在那里。`receive` 返回一条完整的消息，由各个帧拼装而成。空的 optional 表示收到了对端的关闭帧；空消息仍然是一条消息。循环会一直回显，直到客户端关闭。

## 连接并发送

保存为 `ws_client.cpp`：

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

`connect` 建立 TCP 连接并完成 WebSocket 握手。数字端点表示连到哪里；`localhost:9092` 是 HTTP 的 `Host` authority。双方都提供 `echo` 子协议。依次等待发送、接收、关闭：一个 WebSocket 流**同一时间只允许一个异步操作**。如果对端没有在预算内（默认 100 ms）回应关闭，`close_gracefully` 返回 `false`。

## 运行并改变一处行为

构建两个目标。先运行 `xmake run ws_server`，再在另一个终端运行 `xmake run ws_client`。预期输出 `Hello from PACE`；完成关闭握手后两个程序都退出。每运行一次客户端都要重新启动服务器。把客户端的路径改成 `/wrong`，确认服务器拒绝这次升级。

配套程序 [`tutorial_websocket.cpp`](../../examples/tutorial_websocket.cpp) 在一个进程中运行两端。它的客户端还会发送一个 ping，并把文本拆成 4 字节的分片发送，由 `receive` 重新拼装；它的服务器在循环中接受连接。`xmake run tutorial_websocket` 输出 `WebSocket echo verified: Hello from PACE`；`xmake run tutorial_websocket serve` 在 9092 端口上保持服务器直到按下 Ctrl-C，`xmake run tutorial_websocket client` 连接它。

要加密连接，用[第 10 章](10-tls-and-dns.zh.md)的 `tls::connect` 打开一个经过校验的 TLS 流，再把它交给 `websocket::upgrade`；在服务端，`websocket::accept` 同样接受 TLS 流。分片、限制和关闭预算见 [WebSocket 参考](../reference/websocket/README.zh.md) · [协议调优](../guides/protocol-tuning.zh.md)。

下一章：[12. HTTP/2](12-http2.zh.md)
