# 10. 用 HTTP/2 同时提交多个请求

[English](10-http2.md) · [上一章：WebSocket](09-websocket.zh-CN.md) · [下一章：QUIC/HTTP3](11-quic-http3.zh-CN.md)

HTTP/1 章节按顺序复用连接；本章把一批请求放到不同 HTTP/2 stream 上。
先使用明文 prior knowledge，单独理解多路复用，再加 TLS。
启用 `--http2=y`、依赖 `pace_http2`，包含 `<pace/http2.hpp>` 和 `<pace/http_router.hpp>`。

## 第一步：保留路由，只更换协议服务入口

沿用第四章的显式 listener 生命周期，注册路由后改用 http2::serve：

```cpp
pace::http::router app;
app.get("/one", [](pace::http::request) { return pace::http::response{.body = "one"}; });
app.get("/two", [](pace::http::request) { return pace::http::response{.body = "two"}; });
pace::http2::limits limits;
limits.max_concurrent_handlers = 8;
io.spawn(pace::http2::serve(listener, app, limits));
```

request/response 类型不变。实际处理器数量还受 stream 上限约束。
一个 driver 操作 nghttp2 和传输，多个 handler 可以分别等待；
同一 reactor 上的 CPU 代码并不因此多线程并行。需要顺序处理时设为 1。
正常断连之后，已经启动的 handler 可能继续完成，引用捕获必须保持到它们结束。

## 第二步：客户端提交一批请求

在客户端协程中写：

```cpp
pace::tcp::stream transport(io);
co_await transport.connect(endpoint);
std::vector<pace::http::request> requests(2);
requests[0].method = "GET"; requests[0].target = "/one";
requests[1].method = "GET"; requests[1].target = "/two";
const auto replies = co_await pace::http2::exchange(
    std::move(transport), "localhost", std::move(requests));
```

exchange 接管这一批请求所用的连接。响应 vector 按输入顺序排列，
即使第二个处理器先完成也不会换位置。分别检查 status 和 body。
正文仍然完整缓冲，并非流式响应接口。这个明文入口是 prior knowledge，
不要先写 HTTP/1 Upgrade 数据再交给它。

## 第三步：观察并发，再接入 TLS

在仓库启用 HTTP2 后构建运行 `tutorial_http2`，应打印两个响应。
[配套程序](../../examples/tutorial_http2.cpp)将上述监听和批处理接起来。
在你的版本里让 `/one` 等一个定时器、`/two` 立即返回，记录 handler 完成顺序；
日志可以乱序，exchange 返回的 vector 仍应保留请求顺序。

HTTPS 还要启用 `--tls=y`，两端配置 ALPN `h2`，按第八章完成验证握手。
服务端重载接收 TLS 配置，客户端 exchange 接收已握手的 TLS stream。
ALPN 不匹配应当报错，不会悄悄退回 HTTP/1。

让 batch/control 任务完成后再 stop，并把 listener 和 TLS 配置保留在任务外。
流控、半关闭及 handler 生命周期详见 [HTTP/2 参考](../reference/http2.md)。
