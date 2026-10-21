# WebSocket 客户端与服务端

[English API reference](websocket.md) · [TLS 中文指南](tls.zh-CN.md) · [完整示例](../../examples/tutorial_websocket.cpp)

包含 `<pace/websocket.hpp>` 即可使用 RFC 6455 WebSocket。普通 `ws` 不依赖
OpenSSL；启用 `--tls=y` 后可通过 `secure_stream` 使用经过证书验证的 `wss`。
支持 HTTP/1.1 Upgrade、文本与二进制消息、分片、子协议、ping/pong 和关闭握手。

```sh
xmake f -m release --examples=y --tests=y -y
xmake -y
xmake run tutorial_websocket               # 本地自测，完成后退出
xmake run tutorial_websocket serve 9092    # Ctrl-C 停止
xmake run tutorial_websocket client 9092   # 另一终端执行
```

服务端示例仅监听回环地址，路径为 `/echo`，支持 `echo` 子协议。浏览器可使用
`new WebSocket("ws://127.0.0.1:9092/echo", "echo")` 连接。

## 基本用法

```cpp
pace::tcp::stream transport(io);
co_await transport.connect({"127.0.0.1", 9092});
pace::websocket::stream socket(std::move(transport), pace::websocket::role::client);
co_await socket.handshake("localhost", "/echo", {"echo"});
co_await socket.send_text("Hello from PACE", 4); // 按最多 4 字节分片
if (auto reply = co_await socket.receive()) {
    // reply->type 区分 text/binary，reply->data 持有完整消息。
}
const bool completed = co_await socket.close_gracefully();
```

服务端把 `co_await listener.accept()` 得到的 TCP 流移动到 `role::server` 的
WebSocket 对象中，再调用 `accept({"echo"}, authorize)`。授权回调可以检查
`upgrade_request` 的 `target`、`host`、`origin`、`protocols`，返回 false 时
响应 403；回调同步执行，不能阻塞线程。当前不暴露任意 HTTP 请求头。
`selected_protocol()` 返回选中的子协议，没有共同协议时为空；需要强制子协议
的应用应主动检查。服务端按照自己的协议列表顺序选择。

必须在读取 HTTP 字节之前把传输流交给 WebSocket；它自行解析 Upgrade，并保存
与请求头一起收到的首帧。当前它是独立端点，不能直接从 `http::server` 的已解析
请求转换，也没有 HTTP 路由层的 Upgrade 回调。`handshake` 的 Host 与 TCP 数字
地址分开传入，target 必须是以 `/` 开头且不含 URL fragment 的路径和查询串。

## wss 与 TLS

```cpp
pace::tls::context config;
config.set_alpn_protocols({"http/1.1"});
pace::tls::stream transport(io, config);
co_await transport.connect({"192.0.2.1", 443}, "chat.example.com");
pace::websocket::secure_stream socket(std::move(transport), pace::websocket::role::client);
co_await socket.handshake("chat.example.com", "/chat");
```

示例地址仅用于文档，实际使用时换成已解析的服务器地址。TLS 验证证书链和目标
主机名。服务端先包装已接受的 TCP 流并完成 TLS `handshake()`，再包装成
`secure_stream` 并调用 `accept()`。ALPN 只允许空值或 `http/1.1`，不支持
HTTP/2 Extended CONNECT。证书配置见 [TLS 指南](tls.zh-CN.md)。

## 消息、并发与生命周期

`send_text(std::string)`、`send_binary(std::string)` 和 `send(message)` 在挂起
期间持有消息数据；二进制可包含 NUL。可选的分片大小为零时采用帧上限，否则取
传入值与帧上限的较小值。`receive()` 重组完整消息，检查跨帧 UTF-8，自动回复
ping 并消耗 pong。`ping`、`pong` 的负载最多为 125 字节，当前没有 pong 回调。

**一个 WebSocket 流同时只能有一个异步操作，读与写也不能重叠。** 这对 ws 和
wss 都成立。重复操作抛出 `logic_error`，不会取消已有操作。因此当前 API 适合
顺序请求/响应和 echo，不能在一个挂起的 `receive()` 旁独立启动推送协程。
所有操作都在所属 reactor 上执行；对象和 reactor 必须活到操作结束。仅在空闲时
移动对象，`close()` 后也必须等待挂起的成员函数返回再销毁对象。

默认上限：Upgrade 头 16 KiB、数据帧 64 KiB、完整消息 1 MiB、每消息 1024 个
数据帧、每次 receive 1024 个控制帧。构造时可传 `websocket::limits` 修改。
头字段最多 100 个；所有数据/帧计数上限必须大于零，帧大小不能超过消息大小。

实现会拒绝错误掩码方向、保留位或操作码、非最短长度编码、错误 continuation、
非法控制帧、非法 UTF-8、非法关闭状态码及超限消息。`protocol_error::close_code()`
给出 1002（协议）、1007（UTF-8）、1008（控制帧过多）或 1009（大小/分片上限）
等错误，尽可能发送关闭帧后关闭传输。未收到关闭帧就遇到 EOF 属于错误。
网络错误保持 `system_error`，取消使用 `operation_canceled`。应用应在每条连接
内部处理预期错误，避免未捕获异常停止其他连接。

## 关闭与当前范围

`receive()` 收到关闭帧后自动回复、关闭底层 TCP/TLS，并返回 `nullopt`。
`peer_close_code()` 和 `peer_close_reason()` 保存对端状态；没有状态码时返回
空 optional。关闭原因必须是 UTF-8，最多 123 字节。

`send_close(1000, "finished")` 仅发起关闭，需要继续 receive。
`close_gracefully(1000, "finished", 100ms)` 完成整个流程，收到对端关闭帧返回
true，超时返回 false，其他错误继续抛出；无论结果如何都会关闭底层流。
计时是协作式的，无法抢占阻塞的应用代码。提前完成后，内部小计时任务仍会等到
原截止时间或 reactor 停止，但已解除对流对象的引用。`close()` 和析构直接关闭，
不执行协议握手。发送关闭后不能再发送应用消息。

当前不提供压缩、HTTP/2 WebSocket、自动重连、分片流式回调或读写同时运行。
测试包含普通/TLS 往返、恶意输入、取消与计时器生命周期，以及 Python 标准库
独立实现的客户端和服务端互通；尚未取得 Autobahn 完整一致性验证。

```sh
python3 tests/websocket_interop.py /path/to/tutorial_websocket
```
