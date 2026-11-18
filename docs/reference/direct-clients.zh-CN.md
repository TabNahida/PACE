# 直接客户端与 HTTP 命名迁移

[English](direct-clients.md) · [快速入门](../tutorial/00-quick-start.zh-CN.md)

本页 API 需要 **PACE 0.4.0 或更新版本**。HTTP 自由函数的命名有意发生变化；
`app.get`、`router.get` 和持久客户端的 `send` 不变。

## 选择执行方式

| 操作 | 当前线程直接得到结果 | 已有 PACE context 上的异步任务 |
|---|---|---|
| HTTP GET | `http::get(url)` | `co_await http::async_get(url)` |
| HTTP POST | `http::post(url, body)` | `co_await http::async_post(url, body)` |
| 其他 HTTP 方法 | `http::fetch(url, request)` | `co_await http::async_fetch(url, request)` |
| 固定长度 TCP 回复 | `tcp::exchange(peer, request, reply_size)` | `co_await tcp::async_exchange(peer, request, reply_size)` |
| UDP 请求与回复 | `udp::exchange(peer, request)` | `co_await udp::async_exchange(peer, request)` |
| 只发一个 UDP 包 | `udp::send(peer, message)` | 在持有的 UDP socket 上 `co_await socket.send(message, peer)` |

直接调用在当前线程创建独立 context，完成一次收发、关闭自己的 socket，再返回独立数据。
没有隐藏工作线程、连接池或全局事件循环。频繁调用会重复初始化和关闭；高吞吐、并发和连接复用
使用持久异步客户端。在运行中的 PACE 任务或回调内调用直接接口，会在网络 I/O 前抛 `logic_error`，
即使该操作可以立即完成也一样。其他 GUI 或事件循环不在检测范围内，调用仍会阻塞它们的线程。

## 签名

```cpp
// <pace/blocking.hpp>, also included by tcp.hpp, udp.hpp and http.hpp
struct pace::blocking_options {
    std::chrono::milliseconds timeout{30000};
    cancellation_token cancellation{};
    pace::deadline deadline{};
};

// <pace/http.hpp>, link pace_http1; signatures inside namespace pace::http
response get(std::string url, blocking_options wait = {},
             limits bounds = {}, client_options options = {});
response post(std::string url, std::string body,
              std::string content_type = "application/octet-stream",
              blocking_options wait = {}, limits bounds = {}, client_options options = {});
response fetch(std::string url, request outgoing, blocking_options wait = {},
               limits bounds = {}, client_options options = {});
// With PACE_HAS_TLS:
response get(const tls::context& trust, std::string url, blocking_options wait = {},
             limits bounds = {}, client_options options = {});
response fetch(const tls::context& trust, std::string url, request outgoing,
               blocking_options wait = {}, limits bounds = {}, client_options options = {});

// <pace/tcp.hpp>, link pace_net; inside namespace pace::tcp
std::string exchange(endpoint peer, std::string request, std::size_t reply_size,
                     blocking_options options = {}, std::size_t maximum = 1024 * 1024);
task<std::string> async_exchange(io_context&, endpoint, std::string request,
                                 std::size_t reply_size, std::size_t maximum = 1024 * 1024);
task<std::string> async_exchange(endpoint, std::string request,
                                 std::size_t reply_size, std::size_t maximum = 1024 * 1024);

// <pace/udp.hpp>, link pace_net; inside namespace pace::udp
packet exchange(endpoint peer, std::string request, blocking_options options = {},
                std::size_t capacity = 65536);
std::size_t send(endpoint peer, std::string message, blocking_options options = {});
task<packet> async_exchange(io_context&, endpoint, std::string request, std::size_t capacity = 65536);
task<packet> async_exchange(endpoint, std::string request, std::size_t capacity = 65536);
```

## 超时与错误

`blocking_options::timeout` 必须为正，覆盖连接、可选 DNS/TLS、请求、回复及关闭。
零和负数抛 `invalid_argument`。预算使用单调时钟，取 timeout 与绝对 deadline 的较早值；cancellation 可从其他线程取消。
到期后局部操作作用域取消并清理挂起操作，
向调用方抛出可与 `std::errc::timed_out` 比较的 `system_error`。返回后没有遗留后台操作，
下一次调用使用新的独立 context。

这是协作式取消，不能抢占同步的信任库加载、分配、CPU 工作或系统调度，因此实际返回时间可能
超过预算；完成时已超出预算也会报告超时。其他异常照常传播；HTTP 4xx/5xx 仍是正常传输完成的响应。
超时无法撤回已发送的数据或对端已执行的操作。库不会自动重试请求。

`async_` 版本沿用原有异步约定，**不会附加此总超时**。它们继承当前任务的取消与 deadline；
用 [with_options / with_deadline](structured-concurrency.zh-CN.md) 设置局部边界。socket 成员函数仍然是异步的，不要求名字带 `async_`。

## HTTP 与验证过的 HTTPS

URL 处理、消息限制、关闭策略与[异步 URL 客户端](convenience.md#url-client)相同。
数字 HTTP 地址不需要 DNS/TLS；域名需要 `dns=y`，HTTPS 需要 `tls=y`，缺少模块报告 `not_supported`。
HTTPS 默认使用系统信任根验证 URL 主机名。显式 TLS 配置必须在整个调用期间存活：

```cpp
pace::tls::context trust;
trust.load_verify_file("ca.pem");
auto reply = pace::http::get(trust, "https://localhost:8443/health");
```

自定义信任的 POST 可设置 `request.method`、`request.body` 和 Content-Type header，再调用
`fetch(trust, url, request)`。库负责分帧头，URL 助手生成 Host 与 target。
单次调用不自动重定向、代理、解压或复用连接；持久连接见 [HTTP 配方](http-recipes.zh-CN.md)。

## TCP 分帧

地址为数字 IPv4/IPv6，`reply_size` 是恰好要读的长度。超过 `maximum` 会在连接前抛 `length_error`，
提前 EOF 抛 `system_error(connection_reset)`。允许回复长度为零，请求仍会写出；字符串保留内嵌零字节。

读取回复后半关闭发送方向，并按已有的 100 ms / 64 KiB 界限排空；关闭也受总超时约束。
需要持久连接、分隔符或长度前缀、其他关闭策略、复用缓冲区时，使用 `tcp::connect`、`pace::write`
和 `pace::read_exact` 管理 stream。参见[协议调优](protocol-tuning.zh-CN.md)。

## UDP 来源过滤与容量

`exchange` 只发一个包，然后等待来自相同数字 IP、端口、地址族和 IPv6 scope 的回复。
比较前规范化 IP；其他来源会被丢弃且不会重置超时。这不是身份认证、应用请求 ID 匹配或可靠送达。
从不同端点回复的服务需要使用 socket API；广播和组播需要显式配置 socket，本助手面向单播。

`capacity` 必须为 1–65536，发送前验证：零抛 `invalid_argument`，超过上限抛 `length_error`；
目标端口必须非零。匹配但截断的数据报会带着 `truncated=true` 返回，不会静默重试。
零长度数据报合法。`send` 仅返回本地接受的字节数，不等待回复或证明对端收到。每次调用使用新 socket。

## 从 0.3.1 迁移

| 原写法 | 新写法 |
|---|---|
| `pace::run(http::get(url))` | `http::get(url)` |
| `pace::run(http::post(url, body))` | `http::post(url, body)` |
| `co_await http::get(url)` | `co_await http::async_get(url)` |
| `co_await http::post(url, body)` | `co_await http::async_post(url, body)` |
| `co_await http::fetch(url, request)` | `co_await http::async_fetch(url, request)` |
| `co_await http::get(io, peer, target)` | `co_await http::async_get(io, peer, target)` |
| `co_await http::get(io, trust, url)` | `co_await http::async_get(io, trust, url)` |

所有旧 HTTP 自由函数 `get/post/fetch` 重命名为 `async_`，包括显式 context、数字地址和自定义 TLS
重载，参数顺序不变。新的直接 HTTP 接口只接受 URL，没有接收 `io_context&` 的重载。
从 `pace::run(http::get(url, bounds, options))` 迁移为 `http::get(url, {}, bounds, options)`，
注意新增的超时参数位置。

HTTP/2、3 现在采用同样的命名：URL get/post/fetch 同步返回，async_ 前缀为异步；原 transport
形式的 get/exchange 改成 async_get/async_exchange。见[统一客户端](http-clients.zh-CN.md)。
TCP/QUIC connect 和 socket 成员方法名字不变。
