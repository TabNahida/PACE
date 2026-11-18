# 统一 HTTP 客户端

[English](http-clients.md) · [取消与并发契约](structured-concurrency.zh-CN.md)

以下 API 需要 PACE 0.4.0 或更新版本。`pace::http1` 是 `pace::http` 的别名；
HTTP/1、2、3 共用 `http::request` 和 `http::response`。

| 操作 | 直接返回响应 | 协程等待 |
|---|---|---|
| GET | `get(url, wait={}, limits={})` | `co_await async_get(url, limits={})` |
| POST | `post(url, body, content_type, wait={}, limits={})` | `co_await async_post(url, body, content_type, limits={})` |
| 其他方法 | `fetch(url, request, wait={}, limits={})` | `co_await async_fetch(url, request, limits={})` |

这些名字可用于 `pace::http`、`pace::http1`、`pace::http2` 和 `pace::http3`。
Content-Type 默认 `application/octet-stream`。异步 URL 接口也提供首参数为 `io_context&`
的重载。各协议 limits 类型独立；HTTP/1 在 limits 后保留原有 client_options。

直接调用阻塞当前线程，独占新 context 和连接，返回拥有数据的响应；在 PACE context 中调用
会被拒绝。`blocking_options` 包含正数 `timeout`（默认 30 秒）、`cancellation` 和绝对
`deadline`，较早的 deadline 生效。异步调用继承当前任务的取消与 deadline，使用
`with_deadline` / `with_options` 可限制单个请求或整个任务组；异步请求不默认添加总超时。

| 协议 | URL 与传输 | 目标与开关 |
|---|---|---|
| HTTP/1 | http 为 TCP；https 为验证证书的 TLS | pace_http1；http1=y，可选 tls=y |
| HTTP/2 | http 为 prior knowledge；https 验证证书与 h2 ALPN | pace_http2；http2=y，可选 tls=y |
| HTTP/3 | 仅 https，验证 QUIC 证书与 h3 ALPN；拒绝 http | pace_http3；http3=y，仅 POSIX |

数字 IPv4/IPv6 URL 不需要 DNS；域名需要 dns=y，缺少可选支持报告 not_supported。
HTTP/1、2 连接失败时顺序尝试解析得到的 TCP 地址，HTTP/3 使用第一个地址。
不自动降级协议或重试请求。

默认使用系统信任库。私有 CA 使用 `get(trust, url, ...)` / `fetch(trust, url, request, ...)`，
异步使用 `async_get(io, trust, url, ...)` / `async_fetch(io, trust, url, request, ...)`。
HTTP/1、2 的 trust 为 tls::context，HTTP/3 为 quic::context；显式 context 配置 h2/h3 ALPN，
并保持到调用完成。自定义信任的 POST 用 request 设置 method、body、Content-Type 后交给 fetch。

URL 客户端负责 authority 和 target，拒绝 Host/pseudo-header 覆盖。正文按 limits 完整缓冲；
4xx/5xx 正常返回。没有重定向、代理、解压或连接池。多次单次调用会创建多个连接。
HTTP/2、3 同连接多路复用使用 `async_exchange(owned_transport, authority, vector<request>, limits={})`，
结果按请求顺序返回；任一流不完整或 reset 导致整批失败。

## 迁移与并发

原 transport 形式的 `http2::get` / `http3::get` 改名为 `async_get`；`exchange` 改为
`async_exchange`，参数不变。不带前缀的 URL 调用是同步的。
[HTTP/1 迁移](direct-clients.zh-CN.md)说明其之前的重命名；TCP/QUIC connect、socket 成员和路由注册名字不变。

每批只有一个连接 driver，handler 可以在其 context 上交错执行；nghttp2/nghttp3 与传输不被多个线程
同时驱动。serve_connection 退出前取消并等待 handler；serve 停止接收时取消并等待其拥有的连接。
因此关闭 listener 会结束整个所属连接作用域。需要定制优雅排空时，用 with_task_group 自行管理
accept 循环和连接子任务。完整限制见[并发契约](structured-concurrency.zh-CN.md)。
