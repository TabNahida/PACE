# 9. 完成 WebSocket 升级，再收发消息

[English](09-websocket.md) · [上一章：DNS/TLS](08-dns-tls.zh-CN.md) · [下一章：HTTP/2](10-http2.zh-CN.md)

沿用第三章的 accept 循环，把每个连接的字节回显换成 WebSocket 升级和消息回显。
配置 `--websocket=y`，依赖 `pace_websocket`，包含 `<pace/websocket.hpp>`。

## 第一步：在连接任务内接受升级

接受到的 TCP stream 名为 transport，在连接协程中写：

```cpp
pace::websocket::stream socket(std::move(transport), pace::websocket::role::server);
co_await socket.accept({"echo"}, [](const auto& request) {
    return request.target == "/echo";
});
while (auto message = co_await socket.receive())
    co_await socket.send(std::move(*message));
```

角色决定握手和帧处理行为。回调在接受升级前检查路径；业务可在此检查 Origin 和认证。
receive 返回重组后的完整消息，不是某次 TCP read 或某个 continuation frame。
空 optional 表示关闭；正文为空仍可能是一条有效消息。

按第三章把网络异常限制在单个连接，另外捕获 `websocket::protocol_error` 处理
错误升级和非法帧，不要因此让整个 accept 循环退出。

## 第二步：客户端握手、发送、核对

客户端先完成 TCP connect，再写：

```cpp
pace::websocket::stream socket(std::move(transport), pace::websocket::role::client);
co_await socket.handshake("localhost", "/echo", {"echo"});
co_await socket.send_text("Hello from PACE", 4);
const auto reply = co_await socket.receive();
if (!reply || reply->data != "Hello from PACE")
    throw std::runtime_error("WebSocket echo mismatch");
const bool closed = co_await socket.close_gracefully(1000, "finished");
```

这里的 4 让消息分成较小的帧，receive 会重新组装。
closed 表示关闭握手是否在预算内完成；先完成这一步再 stop。
WebSocket 关闭握手和底层 TCP EOF 不是同一件事。

此 API 对同一个 WebSocket stream 只允许一个活动异步操作，收、发、控制帧、关闭
都算在内。不能直接照搬 TCP 的“一读一写并行”做法启动两个独立读写任务。

## 第三步：运行完整流程，再改一个条件

在 PACE 根目录运行：

```sh
xmake f --websocket=y --examples=y -y
xmake -y tutorial_websocket
xmake run tutorial_websocket
```

应输出 `WebSocket echo verified: Hello from PACE`。
[配套源码](../../examples/tutorial_websocket.cpp)把上述两个任务接到第三章的监听和控制结构。
想分进程观察，可在两个终端分别运行 `tutorial_websocket serve` 和 `tutorial_websocket client`。
在副本里改变允许升级的路径，确认错误路径会被拒绝；恢复后再尝试更大的分片消息。
安全 WebSocket 复用第八章的 TLS 配置，类型与限制见[参考](../reference/websocket.md)。
