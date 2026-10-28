# 组件便捷接口（当前 checkout）

[English / 完整签名](component-conveniences.md) · [教程](../tutorial/README.zh-CN.md) · [调优](protocol-tuning.zh-CN.md)

以下新增接口位于 **v0.3.0 之后的源码**，不在该版本发布包里。
请用[第一章](../tutorial/01-build.zh-CN.md)的源码依赖方式。原有底层接口仍然可用，
可选协议继续使用各自的构建开关与模块依赖。

## Context 与生命周期

helper 不启动事件循环、线程或线程池。省略 io 的重载使用 `pace::this_context()`。
异步 helper 在惰性任务开始执行时查找当前 context；同步的 `tcp::listen`、`quic::listen`、`udp::open`
和 `udp::bind` 则在调用时就需要当前 context。想在 io.run 之前构造对象，使用显式 io 重载。
事件循环外可将惰性任务交给 run，例如 `pace::run(dns::resolve("127.0.0.1"))`。

按值参数由任务持有；stream 引用、span 和自定义 TLS 配置仍然是借用，必须保持到 await
完成且不能移动。context 必须活得比其绑定对象久，不能从 `pace::run` 返回绑定于其内部
context 的 socket。helper 不提供额外锁、操作队列、通用期限、连接池或重试。

## 字节流与 TCP

`<pace/stream_io.hpp>` 定义通用接口，TCP/TLS/QUIC 头文件也会包含它：

```cpp
co_await pace::write(stream, std::string("a\0b", 3));
auto data = co_await pace::read_exact(stream, size, maximum);
co_await pace::read_exact(stream, std::span<char>(buffer));
```

适用于 tcp::stream、tls::stream 和 quic::stream。write 持有字符串，左值会复制，
移动字符串则转移所有权。read_exact 只读取所需长度，不消耗后一条消息；默认 maximum
为 1 MiB，超限在分配和 I/O 前抛 length_error。span 重载也支持 std::byte，使用调用方
缓冲区，没有额外长度上限。零长度直接完成，提前 EOF 抛 system_error(connection_reset)。
失败时 span 的前缀可能已经改变；取消与 I/O 错误继续传播。
它们不定义消息边界，也不保证对端业务处理完毕。

`tcp::listen([io,] endpoint, options)` 立即绑定并返回 listener；已有的
`tcp::connect([io,] endpoint)` 返回已连接 stream。省略 io 使用当前 context。
endpoint 是数字 IPv4/IPv6，不隐含 DNS。原有成员 read_exact 保留。[TCP 契约](tcp.md)。

## UDP

依赖 pace_net，包含 `<pace/udp.hpp>`：

```cpp
auto receiver = pace::udp::bind({"127.0.0.1", 0});
auto sender = pace::udp::open();
co_await sender.send("hello", receiver.local_endpoint());
auto packet = co_await receiver.receive();
```

open 返回未绑定 socket，首次发送选择地址族；bind 立即绑定，端口 0 由系统分配。
两者都有显式 io 重载。send 持有正文，返回发送字节数。receive 返回拥有 data 的 packet，
还带 sender 与 truncated。默认分配 65536 字节容量；参数为 0 抛 invalid_argument，
超过 65536 抛 length_error，均在 I/O 之前。较小容量可能截断，剩余数据已丢弃，必须检查。
默认容量不承诺网络能承载 64 KiB 数据报。零长度数据报有效。

同一个 socket 可同时一发一收，不可同时两发或两收。关闭取消等待，但要等操作退栈后
再销毁对象。send_to/receive_from 仍借用调用方缓冲区，适合复用。[UDP 契约](udp.md)。

## DNS

启用 --dns=y、链接 pace_dns，包含 `<pace/dns.hpp>`。
`dns::resolve([io,] hostname, family = any, options = {})` 持有 resolver、域名和配置，
返回与 resolver::resolve 相同的 result。默认系统 nameserver、5 秒完整查询期限、两次尝试。
数字地址不需要 nameserver；context.stop 会取消等待，DNS 原始错误类别保留。
需要单独 cancel() 句柄时使用显式 resolver。它不选择端口、不连接、不自动尝试多个地址。
[DNS 契约](dns.md)。

## TLS

启用 --tls=y、链接 pace_tls，包含 `<pace/tls.hpp>`。
`tls::connect([io,] config, endpoint, expected_name)` 返回握手完成后的 stream；
省略 config 使用 helper 自己持有的系统信任配置，不设置 ALPN，也不下载根证书。
`tls::accept(tcp_stream, config)` 接管 TCP 连接并完成服务端握手。

自定义配置**借用到任务完成**，不能保存引用已销毁临时配置的惰性任务。
返回的 stream 持有共享配置状态。首次使用前完成信任和 ALPN 配置。
证书身份与数字 endpoint 分开，DNS 名还会设置 SNI；没有跳过验证模式。
同一个 TLS stream 最多一个异步操作，读写也不能并行。[TLS 契约](tls.md)。

## WebSocket

启用 --websocket=y、链接 pace_websocket，包含 `<pace/websocket.hpp>`。
`websocket::connect([io,] endpoint, authority, target, protocols, limits)` 一次完成 TCP
连接与客户端握手；后三项默认 `/`、空协议列表、默认限额。
`upgrade(transport, authority, target, protocols, limits)` 接受已有 TCP 或已握手 TLS。
`accept(transport, protocols, authorize, limits)` 完成服务端握手，后三项均可省略。
TLS 传输还需要 --tls=y。

参数及连接由任务持有，authorize 捕获的引用仍由调用方保证寿命。
authority 是 Host，可带端口；不是 URL，不解析 ws://，不隐含 DNS。
省略 authorize 意味着没有业务授权检查。需要完整升级元数据时，使用返回 upgrade_request
的成员 accept；便捷函数只返回 socket。一个 stream 只允许一个活动异步操作，包括收发
及关闭。[WebSocket 契约](websocket.md)。

## QUIC

启用 --quic=y、链接 pace_quic，包含 `<pace/quic.hpp>`；当前要求 POSIX。
`quic::connect([io,] config, endpoint, expected_name, timeout = 10s)` 持有轻量共享配置
的副本，完成验证握手后返回 connection。信任与 ALPN 仍需事先明确配置。
`quic::listen([io,] endpoint, server_config)` 同步绑定，返回持有共享配置的 listener。
timeout 只覆盖建立连接，不是 stream 或请求期限；非正值允许一次立即握手尝试。
连接必须保持打开直到其 stream 操作结束，服务端 listener 也要保持到连接结束。
[QUIC 契约](quic.md)。

## HTTP/2 与 HTTP/3

启用对应选项，链接 pace_http2 或 pace_http3：
`http2::get(transport, authority, target = "/", limits = {})` 与
`http3::get(connection, authority, target = "/", limits = {})`
接管连接，委托 exchange 发出单个 GET 并返回首个响应。
HTTP/2 接受 prior-knowledge TCP 或 ALPN 为 h2 的已验证 TLS；HTTP/3 接受 ALPN 为 h3
的已验证 QUIC。这里是 authority/路径接口，没有 URL 解析、DNS 或协议回退。
正文完整缓冲，原有限额不变。自定义头、其他方法及多个并发请求使用 exchange。
GET 完成后调用方不能再使用已经转移的连接。
[HTTP/2](http2.md) · [HTTP/3](http3.md)。
