# PACE 文档

使用 C++20 协程构建异步网络应用。

[项目首页](../README.zh-CN.md) · [English](README.md) · [API 索引](reference/README.zh-CN.md)

## 从这里开始

```sh
xmake f -m release -y
xmake -y tutorial_tcp
xmake run tutorial_tcp
```

示例启动本地 TCP 服务端和客户端，交换消息后自行退出。
需要 xmake 2.9.8+ 和支持 C++20 的编译器。

[构建与集成](tutorial/01-build.md) → [任务与定时器](tutorial/02-tasks-timers.md) → [TCP](tutorial/03-tcp.md) → [工作线程](tutorial/05-execution.md) → [有序关闭](tutorial/06-shutdown.md)

## 按层选择模块

只链接应用需要的模块，核心层没有第三方依赖。

| 层级 | 使用指南 | 可运行示例 |
|---|---|---|
| **运行时** | [任务](reference/task.md) · [事件循环](reference/io-context.md) · [工作线程](reference/runtime.md) | [任务](../examples/tutorial_tasks.cpp) · [工作线程](../examples/tutorial_workers.cpp) |
| **网络核心** | [TCP](tutorial/03-tcp.md) · [UDP](reference/udp.md) | [TCP](../examples/tutorial_tcp.cpp) · [UDP](../examples/tutorial_udp.cpp) |
| **基础协议扩展** | [TLS](reference/tls.md) · [DNS](reference/dns.md) · [QUIC](reference/quic.md) | [TLS](../examples/tutorial_tls.cpp) · [DNS](../examples/tutorial_dns.cpp) · [QUIC](../examples/tutorial_quic.cpp) |
| **应用协议** | [HTTP/1.1](reference/http.md) · [HTTP/2](reference/http2.md) · [HTTP/3](reference/http3.md) · [WebSocket](reference/websocket.md) | [HTTP](../examples/tutorial_http.cpp) · [HTTP/2](../examples/tutorial_http2.cpp) · [HTTP/3](../examples/tutorial_http3.cpp) · [WebSocket](../examples/tutorial_websocket.cpp) |

HTTP/1.1 和 WebSocket 默认启用；TLS、DNS、QUIC、HTTP/2、HTTP/3 按需开启，
详见[构建选项](tutorial/01-build.md)。TLS/QUIC 示例使用 `tests/certs` 中公开的测试专用证书。

## 了解设计

- [架构](architecture.md)：分层、事件循环、所有权与调度。
- [API 参考](reference/README.zh-CN.md)：签名、错误处理与并发约定。
- [教程](tutorial/README.zh-CN.md)：完整运行命令和示例。
- [性能](performance.md)：本地 Linux 完整数据、取舍与复现方法。
- [平台支持](reference/platform.md)：各平台后端与支持的配置。

TCP、UDP 与 QUIC 支持数字 IPv4/IPv6 地址，DNS 可解析 A 和 AAAA。
HTTP/1 支持请求头准入、逐请求 body 限额和有界流式响应；HTTP/2、HTTP/3 的 body 仍采用缓冲方式。
各协议的具体边界见对应参考页。
