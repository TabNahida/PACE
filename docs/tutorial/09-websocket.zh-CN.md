# 9. 连接并交换 WebSocket 消息

[English](09-websocket.md) · [上一章：DNS/TLS](08-dns-tls.zh-CN.md) · [下一章：HTTP/2](10-http2.zh-CN.md)

使用当前 checkout，启用 `--websocket=y`，依赖 `pace_websocket`。
沿用第三章的两个 target 写法，文件与目标分别命名为 `ws_server`、`ws_client`。
本练习只接收**一个连接**，完成后两端退出。

## 第一步：接受升级

编写 ws_server.cpp：

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

`websocket::accept` 接管 TCP stream 并完成服务端握手。
回调允许 `/echo`；可以在这里增加业务需要的 Origin 或身份检查。
`receive` 返回重组后的完整消息。空 optional 表示收到关闭握手，空正文仍然是消息。
循环持续回显，直到客户端关闭。

## 第二步：连接并发送

编写 ws_client.cpp：

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

`connect` 一次完成 TCP 连接和 WebSocket 握手。数字 endpoint 决定连接地址，
`localhost:9092` 是 HTTP Host，双方都声明 `echo` 子协议。
依次 await 发送、接收、关闭：同一个 WebSocket stream 只允许**一个活动异步操作**。

## 第三步：运行并改变一个条件

构建两个目标，先运行 `xmake run ws_server`，另一个终端运行 `xmake run ws_client`。
应输出 `Hello from PACE`，两端在关闭握手后退出。再次连接前重新启动服务端。
把客户端路径改成 `/wrong`，确认升级被拒绝。持续接收多个客户端与协作式信号关闭见
`xmake run tutorial_websocket serve` 及其[源码](../../examples/tutorial_websocket.cpp)。

增加加密时，用第八章的 `tls::connect` 返回的已验证连接调用 `websocket::upgrade`；
服务端 `accept` 同样接受 TLS stream。
[接口签名](../reference/component-conveniences.zh-CN.md#websocket) ·
[分片、限额与关闭预算](../reference/protocol-tuning.zh-CN.md#websocket)
