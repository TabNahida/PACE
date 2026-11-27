# PACE 文档

先从[一次调用的客户端](tutorial/02-http-client.zh.md)开始：直接获取 HTTP 响应，再尝试 TCP、UDP。这些接口需要 PACE 0.4.0 或更新版本。

从[逐步学习路线](tutorial/README.zh.md)开始：创建自己的 main.cpp，逐步加入网络和协议，
每章核对输出并解释生命周期。配套 example 用于验证，不代替教程正文。

使用 C++20 协程构建异步网络应用。

[项目首页](../README.zh.md) · [English](README.md) · [API 索引](reference/README.zh.md)

## 从这里开始

按[第一章](tutorial/01-install-and-first-program.zh.md)创建自己的 main.cpp 和 xmake.lua，
先使用 HTTP、TCP、UDP 的直接客户端，需要时再加入异步任务。每章有构建步骤、预期结果和可修改的练习。

[入门](tutorial/02-http-client.zh.md) → [HTTP](tutorial/03-http-server.zh.md) / [TCP](tutorial/05-tcp.zh.md) / [UDP](tutorial/06-udp.zh.md) → [任务](tutorial/04-tasks-and-timers.zh.md) → [线程](tutorial/08-workers-and-threads.zh.md) → [关闭](tutorial/09-graceful-shutdown.zh.md)

## 按层选择模块

只链接应用需要的模块，核心层没有第三方依赖。

| 层级 | 使用指南 | 可运行示例 |
|---|---|---|
| **运行时** | [任务](reference/runtime/task.md) · [事件循环](reference/runtime/io-context.md) · [工作线程](reference/runtime/runtime.md) | [任务](../examples/tutorial_tasks.cpp) · [工作线程](../examples/tutorial_workers.cpp) |
| **网络核心** | [TCP](tutorial/05-tcp.zh.md) · [UDP](tutorial/06-udp.zh.md) | [TCP](../examples/tutorial_tcp.cpp) · [UDP](../examples/tutorial_udp.cpp) |
| **基础协议扩展** | [TLS](tutorial/10-tls-and-dns.zh.md) · [DNS](tutorial/10-tls-and-dns.zh.md) · [QUIC](tutorial/13-quic-and-http3.zh.md) | [TLS](../examples/tutorial_tls.cpp) · [DNS](../examples/tutorial_dns.cpp) · [QUIC](../examples/tutorial_quic.cpp) |
| **应用协议** | [HTTP/1.1](tutorial/03-http-server.zh.md) · [HTTP/2](tutorial/12-http2.zh.md) · [HTTP/3](tutorial/13-quic-and-http3.zh.md) · [WebSocket](tutorial/11-websocket.zh.md) | [HTTP](../examples/tutorial_http.cpp) · [HTTP/2](../examples/tutorial_http2.cpp) · [HTTP/3](../examples/tutorial_http3.cpp) · [WebSocket](../examples/tutorial_websocket.cpp) |

HTTP/1.1 和 WebSocket 默认启用；TLS、DNS、QUIC、HTTP/2、HTTP/3 按需开启，
详见[构建选项](tutorial/01-install-and-first-program.zh.md)。TLS/QUIC 示例使用 `tests/certs` 中公开的测试专用证书。

## 了解设计

- [架构](explanation/architecture.md)：分层、事件循环、所有权与调度。
- [API 参考](reference/README.zh.md)：签名、错误处理与并发约定。
- [教程](tutorial/README.zh.md)：从自己的文件开始，逐步构建应用并核对结果。
- [性能](performance/README.md)：本地 Linux 完整数据、取舍与复现方法。
- [平台支持](reference/platform-support.md)：各平台后端与支持的配置。

TCP、UDP 与 QUIC 支持数字 IPv4/IPv6 地址，DNS 可解析 A 和 AAAA。
HTTP/1 支持请求头准入、逐请求 body 限额和有界流式响应；HTTP/2、HTTP/3 的 body 仍采用缓冲方式。
各协议的具体边界见对应参考页。

[直接客户端、超时与 HTTP 命名迁移](reference/direct-clients.zh.md)

[搭建静态服务与反向代理](tutorial/14-gateway.zh.md) · [nginx 性能对照](performance/history/gateway-optimization-2026-11-03.md)
