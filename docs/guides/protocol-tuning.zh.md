# 协议调优与显式控制

[English](protocol-tuning.md) · [入门教程](../tutorial/README.zh-CN.md) · [便捷接口契约](component-conveniences.zh-CN.md)

先用教程跑通应用，再根据实际负载调整。便捷包装保留传输语义，持有数据所增加的分配
本身不是性能优化。比较性能时记录正文大小、连接数、线程数、编译选项和多次测量波动，
将插桩 profiling 与正式计时分开，见[本地测试方法](../../benchmarks/README.md)。

## TCP 与 UDP

所有权优先时使用 pace::write 和返回字符串的 pace::read_exact。
热点路径可复用缓冲区，使用 write_all(string_view)、read_some(span)，等 await 完成后
才修改缓冲区。它们借用数据，stream 和存储都要在等待期间保持有效且地址稳定。
精确读取仍需要协议定义分帧；不要未经验证就按对端声明的长度分配。

UDP receive 每次按 capacity 分配，默认 64 KiB。高频收包使用 receive_from(span)
复用存储，并检查 size、sender、truncated。send_to 避免持有字符串的复制，但视图仍要
活到 await 完成。两种接口都不增加可靠送达或重试。

TCP 不能同时两读或两写，UDP 不能同时两发或两收。TCP 的一读一写并行规则不适用于
TLS 与 WebSocket。set_no_delay 在合包和延迟之间取舍，应按负载测量。
IPv6 listener 默认只接受 IPv6；测试使用端口 0 配合 local_endpoint。
backlog 由 tcp::listen_options 指定，不要为每条连接创建新事件循环。
[TCP](tcp.md) · [UDP](udp.md) · [取消与关闭](../tutorial/06-shutdown.zh-CN.md)。

## DNS 与 TLS

一次性 resolve 持有一个查询。显式 dns::resolver 适合保留 cancel 句柄或顺序复用包装对象；
不能据此推断存在持久 DNS 缓存或 channel 池。每个 resolver 同时一个查询，独立实例可以
重叠等待。options 配置数字 nameserver、重试与完整查询期限，不是 connect 期限。
系统 resolver/hosts 配置可能有同步读取，不存在 helper 线程自动把所有初始化变成异步。
[DNS 细节](dns.md)。

多条 TLS 连接可复用已经配置的 context，避免每次默认 connect 都载入信任根。
配置在首次构造 stream 或注册 SNI 目标后冻结；stream 持有共享状态，惰性 helper 则
借用自定义配置对象到 await 完成。SNI 与仅使用应用信任库仍需显式设置。
HTTP/2 使用 ALPN h2，HTTP/1 协商时用 http/1.1，裸 TLS 则设置自己的协议。
不要用关闭证书验证处理身份不匹配。[TLS 细节](tls.md)。

## WebSocket

limits 限制升级头、帧字节、消息字节、分片数和一次 receive 处理的控制帧数。
send_text/send_binary 持有正文，fragment_bytes 为 0 时使用帧上限；只有明确需要时
再降低分片大小，过多分片增加处理成本。receive 在限额内重组消息、处理 ping/pong。

同一 stream 的收、发、控制帧、关闭总共最多一个活动操作，不能启动独立读写任务。
内部协议 driver 的规则不代表应用能双向并发调用公共 TLS/WebSocket 方法。
close_gracefully 超时返回 false，并始终关闭传输；裸 EOF 不等于成功关闭握手。
升级授权使用回调，便捷入口不会默认替应用检查身份。[限额与关闭契约](websocket.md)。

## HTTP1

单次 URL GET 适合起步；连续访问改用 http::client 或 secure_client 顺序复用连接，
同时只能有一个 send。进阶 handler 可以在头验证后决定准入、设置每条路由的 body 上限、
设置收发阶段期限并按块生成响应。这些不等于全局连接数/内存上限，也不能抢占同步业务代码。
默认客户端在完整响应分帧后不再等待对端 FIN；确有需要时才启用有界 graceful 关闭。
[HTTP 配置与进阶用法](http-recipes.zh-CN.md)。

## HTTP2 与 HTTP3

get 持有一个连接完成单个 GET 批次。多路复用时，将多个拥有内容的请求一次提交：

```cpp
std::vector<pace::http::request> requests(2);
requests[0].method = "GET"; requests[0].target = "/one";
requests[1].method = "GET"; requests[1].target = "/two";
auto replies = co_await pace::http2::async_exchange(
    std::move(transport), "localhost", std::move(requests));
```

HTTP/3 形式相同，使用已验证的 QUIC connection。结果保持输入顺序，正文仍按流限额
完整缓冲；不会创建连接池，也不是增量流式 body API。

一个 driver 操作协议 session 与 transport，handler 可以分别挂起。
实际并发上限是 max_concurrent_handlers 和 max_concurrent_streams 的较小值，默认均为 100。
设 handler 上限为 1 可顺序处理；调高可能增加内存与下游负载，并不会增加 CPU 线程。
CPU 并行使用显式 runtime worker，并同步共享状态。连接退出会取消并等待已启动的 handler；
关闭 listener 会取消并等待已接受的连接。引用必须存活到对应边界结束。
详见[局部取消与清理屏蔽](structured-concurrency.zh-CN.md)。

HTTP/2 TLS 必须验证 ALPN h2，HTTP/3 为 h3，不匹配则失败而不是自动回退。
调整流控和消息边界前查阅 [HTTP/2](http2.md) 与 [HTTP/3](http3.md)。

## QUIC

首次连接前配置身份和 ALPN。quic::connect 默认握手期限 10 秒，与业务请求/stream
期限分开。finish_write 只结束一个流的发送方向，对端仍能回复。
stream 操作期间保持 connection 打开；服务端 listener 也要活到其连接操作结束。
pace::write/write_all 完成只表示交给传输，不表示确认收到。销毁发送方前，按
[配套程序](../../examples/tutorial_quic.cpp)在 pending_write_bytes 非零时继续 await 进展。
立即 connection.close 是尽力发送的关闭，不是送达确认。

HTTP/3 接管 QUIC 连接，包括控制流和 QPACK，不能再在转移后的连接上自行开业务流。
当前未公开 0-RTT、WebTransport、CONNECT 或 server push。[QUIC 细节](quic.md)。

本地 QUIC 交换中，客户端读到回复 FIN 后仍保持存活，直到服务端观察到确认包。
配套程序通过等待 connection 事件保留客户端到两端完成；收到 FIN 不等于远端发送任务已经结束。
