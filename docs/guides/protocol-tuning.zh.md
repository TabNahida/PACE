<!-- source-hash: 6be91f86c11e -->
# 协议调优与显式控制

[English](protocol-tuning.md) · [指南](README.zh.md) · [教程](../tutorial/README.zh.md) · [API 参考](../reference/README.zh.md)

先用简单接口把应用跑通，再针对实际负载测量，然后才调整这些参数。持有数据的便捷调用保留了传输层的行为；
它们为持有数据而多做的分配本身并不是性能优化。插桩 profiling 与正式计时要分开进行，并记录正文大小、
连接数、线程数、构建选项以及多轮测量之间的波动。参见[本地基准测试方法](../../benchmarks/README.md)。

## TCP 与 UDP

更看重简单的所有权时，使用 `pace::write` 和返回持有数据的 `pace::read_exact`。
热点路径上可以保留一块缓冲区，等 `stream.write_all(string_view)` 或 `read_some(span)` 完成后再复用。
这些调用是借用的：缓冲区和 stream 在挂起期间都必须存活且地址不变。精确读取仍需要应用自己的分帧规则；
按不可信的长度分配内存之前，先检查上限。

`udp::socket::receive` 每次调用都按 capacity 分配（默认 64 KiB）。
高频收包时用 `receive_from(span)` 复用存储，并检查 `size`、`sender` 和 `truncated`。
`send_to` 省去了一次持有字符串的复制，但它的视图必须活到 await 完成。两种形式都不提供送达保证或重试。

TCP 上不要同时进行两个读或两个写，UDP 上不要同时进行两个发送或两个接收。
TCP 允许一读一写并行，但这条规则不适用于 TLS 和 WebSocket。
`set_no_delay()` 用 TCP 合包换取延迟，应结合实际正文大小和流量模式测量。IPv6 listener 默认只接受 IPv6；
测试中使用端口 0 加 `local_endpoint()`。backlog 通过 `tcp::listen_options` 显式调整，
不要为每个连接新建一个事件循环。
[TCP](../reference/net/tcp.zh.md) · [UDP](../reference/net/udp.zh.md) · [取消](../tutorial/09-graceful-shutdown.zh.md)。

## DNS 与 TLS

一次性的 `dns::resolve` 持有一次查询。需要保留 `cancel()` 句柄、或者顺序复用同一个对象做多次查询时，
显式持有一个 `dns::resolver`；这种复用并不意味着有持久的 DNS 缓存或连接池化的 channel。
一个 resolver 同一时间只支持一次查询，独立的 resolver 实例可以同时等待。`dns::options` 配置数字形式的
nameserver、重试次数和整次查询的期限（默认 5 秒、2 次），它不是连接期限。读取系统 resolver 和 hosts
配置可能是同步的；并没有辅助线程把每个初始化步骤都变成异步。[DNS 参考](../reference/dns/README.zh.md)。

多条连接复用同一个已配置的 `tls::context`，而不是每次默认 `tls::connect` 都重新载入信任根。
配置在首次构造 stream、注册为 SNI 目标或被 ClientHello 回调选中时冻结。stream 持有共享状态，
但惰性 helper 会借用自定义配置，直到其 await 完成。SNI 路由和仅使用应用信任库都需要显式设置。
HTTP/2 配置 ALPN `h2`，HTTP/1 在协商时用 `http/1.1`，裸 TLS 则使用自己的协议名。
不要用关闭证书验证的办法绕过名称不匹配。[TLS 参考](../reference/tls/README.zh.md)。

## WebSocket

`websocket::limits` 限制升级请求头、帧字节数、消息字节数、分片数，以及一次 receive 处理的控制帧数量。
`send_text` 和 `send_binary` 持有各自的正文；`fragment_bytes = 0` 表示使用 `max_frame_bytes`。
只有负载或对端确实需要时才调小分片大小：分片过多会增加分帧开销。
`receive` 在限额内重组完整消息并处理 ping/pong。

每个 stream 同一时间只能有一个活动操作，发送、接收、ping/pong 和关闭都算在内。
不要在公共接口上分别运行独立的读任务和写任务。内部协议 driver 有不同的规则，但这并不让应用调用获得双向并发。
`close_gracefully` 在预算耗尽时返回 false，并且总会关闭传输；裸 EOF 不算成功的 WebSocket 关闭握手。
升级策略通过授权回调实现；简单 helper 不会替你安装这个回调。[WebSocket 参考](../reference/websocket/README.zh.md)。

## HTTP/1

一次性的 URL GET 是不错的起点。需要按顺序复用 keep-alive 连接时，使用 `http::client` 或 `secure_client`；
同一时间只能有一个 send。返回 `server_response` 的 handler 可以在请求头验证之后准入或拒绝请求、
为每条路由选择 body 上限、设置接收和写入期限，并通过有界的 producer 流式发送响应。
这些都不是全局的连接数或内存上限，也不能抢占同步执行的应用代码。默认的最终响应关闭策略在分帧完整后
不再等待对端 FIN；只有确实需要这种取舍时，才启用有界的优雅关闭。[HTTP 配置与进阶用法](http-recipes.zh.md)。

## HTTP/2 与 HTTP/3

`async_get` 为一次 GET 持有一个连接。要多路复用，就在同一个连接上把多个持有数据的请求一起提交给
`http2::async_exchange` 或 `http3::async_exchange`（`<pace/http2.hpp>`，目标 `pace_http2`，`--http2=y`）：

```cpp
std::vector<pace::http::request> requests(2);
requests[0].method = "GET"; requests[0].target = "/one";
requests[1].method = "GET"; requests[1].target = "/two";
auto replies = co_await pace::http2::async_exchange(
    std::move(transport), "localhost", std::move(requests));
```

`transport` 是已连接的 `tcp::stream`（prior knowledge 方式）或已验证的 `tls::stream`。
HTTP/3 形式相同，使用已验证的 QUIC 连接。结果保持输入顺序；响应 body 仍按每个流的上限完整缓冲。
不会创建连接池，这些也不是增量流式 body 接口。

一个 driver 持有协议会话和传输。应用 handler 可以各自挂起；实际并发上限是 `max_concurrent_handlers`
与 `max_concurrent_streams` 中的较小值（HTTP/2 默认均为 100，HTTP/3 默认均为 32）。把 handler 上限设为 1
即可顺序分派。调高上限可能增加内存占用和同时进行的下游工作，但不会增加 CPU 线程。CPU 并行请使用显式的
runtime worker，并同步 handler 之间共享的状态。连接退出前会取消并等待活动中的 handler；关闭 listener
会取消并等待已接受的连接。借用的引用必须活到这个边界。局部取消与屏蔽见
[结构化并发](../reference/runtime/structured-concurrency.zh.md)。

基于 TLS 的 HTTP/2 要求已验证的 ALPN `h2`，HTTP/3 要求 `h3`。不匹配时直接失败，而不是回退。
调整流控窗口或消息上限之前，请先阅读 [HTTP/2](../reference/http2/README.zh.md) 和
[HTTP/3](../reference/http3/README.zh.md) 参考。

## QUIC

在第一次连接之前配置信任和 ALPN。`quic::connect` 默认的握手预算为 10 秒，与应用的请求或流期限相互独立。
`finish_write()` 为一个流的单个方向发送 FIN，对端仍然可以回复。流在操作期间要保持连接存活；
listener 也要活到其接受的连接结束。`pace::write` / `write_all` 只是把字节交给传输层，并不代表对端已经确认。
销毁发送方之前，只要 `pending_write_bytes()` 不为零就继续 await 进展，做法见
[QUIC 示例](../../examples/tutorial_quic.cpp)。立即调用 `connection.close()` 只是尽力而为，不是送达确认。

在本地 QUIC 交换中，客户端读到回复的 FIN 之后仍保持存活，直到服务端观察到它的确认。
示例会一直等待连接事件，直到两端都完成；仅收到 FIN 并不等于远端发送方已经结束。

HTTP/3 接管交给它的 QUIC 连接，包括控制流和 QPACK 流；不要再在这个连接上自行打开应用流。
该接口不提供 0-RTT、WebTransport、CONNECT 或 server push。[QUIC 参考](../reference/quic/README.zh.md)。

## worker 布局

worker 数量应通过测量真正关心的负载来确定，并为客户端、内核和任何阻塞线程留出 CPU。
正文大小、连接数、编译选项和硬件布局要保持一致。runtime 既不提供 CPU 亲和性，也不提供自适应线程池。
worker 数等于逻辑 CPU 数，并不能证明吞吐量最大。

在支持 reuse-port 的平台上处理入站 TCP 和 HTTP 时，[`echo_server`](../../examples/echo_server.cpp) 和
[`http_server`](../../examples/http_server.cpp) 示例用 `tcp::listen_options{.reuse_port = true}`
为每个上下文在同一端口上创建一个 listener，由操作系统分配新连接。PACE 在 Windows 上不支持这种做法，
因此这两个示例在 Windows 上只使用一个 worker；多个上下文使用不同端口或发起出站连接仍然可行。
不要为了均衡负载把活动中的 socket 移到另一个上下文。参见 [worker 与线程](../tutorial/08-workers-and-threads.zh.md)。
