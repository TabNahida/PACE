# 11. 从 QUIC stream 走到 HTTP/3

[English](11-quic-http3.md) · [上一章：HTTP/2](10-http2.zh-CN.md) · [学习路线](README.zh-CN.md)

QUIC 在 UDP 上提供经过认证的可靠多路流；HTTP/3 再增加 HTTP 分帧和 QPACK。
先完成 QUIC 流，再交给 HTTP/3 适配层，不要把裸 UDP 数据报当成 QUIC，
也不要把任意 QUIC stream 字节当成 HTTP/3。当前后端需要 POSIX 平台。

## 第一步：先配置身份和应用协议

启用 `--quic=y`，依赖 `pace_quic`，包含 `<pace/quic.hpp>`。
和 TLS 练习一样准备证书、私钥和信任，但使用 quic::context：

```cpp
pace::quic::context server_config(pace::quic::role::server), client_config;
server_config.use_certificate_chain("tests/certs/localhost.pem", "tests/certs/localhost-key.pem");
client_config.load_verify_file("tests/certs/ca.pem");
server_config.set_alpn_protocols({"pace-demo"});
client_config.set_alpn_protocols({"pace-demo"});
pace::quic::listener listener(io, {"127.0.0.1", 0}, server_config);
```

把配置和 listener 放在任务外，保持到 io.run 返回。上述相对路径以仓库根为准，
证书仅用于本地练习。QUIC 自己完成 TLS 1.3 握手，不要求启用单独的 TCP TLS 模块。

## 第二步：打开一个流，明确结束发送方向

客户端协程中写：

```cpp
pace::quic::connection connection(io, client_config);
co_await connection.connect(endpoint, "localhost");
auto stream = co_await connection.open_stream();
co_await stream.write_all("hello");
stream.finish_write();
```

服务端先 await listener.accept，再 await connection.accept_stream。
读取到这个 stream 的 EOF 后写回复，并结束回复的发送方向。
FIN 针对一个 stream，不是关闭连接上的全部流。连接对象要活得比流的操作久。

write_all 把数据交给传输，并不等于已被确认；若要在销毁前等完排队数据，
在 pending_write_bytes() 非零时继续 await 流事件。
connection.close() 是尽力发送的立即关闭，不是可靠送达确认。
[QUIC 配套程序](../../examples/tutorial_quic.cpp)等两端完成后才 stop，并有本地期限。
运行 tutorial_quic 应输出 `Hello over verified QUIC!`；之后可尝试增加第二个流，
观察第一个流结束不会自动结束第二个。

## 第三步：让 HTTP/3 接管协议流

启用 `--http3=y`，依赖 `pace_http3`；两端 ALPN 改成 `h3`。
包含 `<pace/http3.hpp>` 和 `<pace/http_router.hpp>`，保留前面章节的路由：

```cpp
pace::http::router app;
app.get("/health", [](pace::http::request) { return pace::http::response{.body = "ok"}; });
pace::http3::limits limits;
limits.max_concurrent_handlers = 8;
io.spawn(pace::http3::serve(listener, app, limits));
```

客户端用 h3 配置完成验证连接，像第十章一样组好 request vector，再转移连接：

```cpp
const auto replies = co_await pace::http3::exchange(
    std::move(connection), "localhost", std::move(requests));
```

连接转移后，不再自行打开业务或控制流；HTTP/3 适配层持有控制流和 QPACK 状态。
返回结果保留输入顺序，正文仍缓冲，handler 仍是同一 reactor 上的协作式并发。
正常断连不会强行销毁已运行的 handler，引用捕获必须保持到完成；
需要取消 PACE 等待时通过 context.stop。

## 第四步：运行并验证协议协商

在仓库根执行：

```sh
xmake f --http3=y --examples=y -y
xmake -y tutorial_quic tutorial_http3
xmake run tutorial_quic
xmake run tutorial_http3
```

[HTTP/3 配套程序](../../examples/tutorial_http3.cpp)打印 `/one`、`/two` 两个响应。
在副本里把一端 ALPN 改错，确认握手失败：UDP 地址能访问并不代表已建立认证 HTTP/3 会话。
当前没有公开 server push、CONNECT、WebTransport 或 0-RTT 接口。
边界与资源限制见 [QUIC 参考](../reference/quic.md) 和 [HTTP/3 参考](../reference/http3.md)。
