# PACE 文档

先从 [0.3.1 快速入门](tutorial/00-quick-start.zh-CN.md)开始：安装包、几行启动 HTTP 服务，再逐步加入异步等待和 URL 客户端。

从[逐步学习路线](tutorial/README.zh-CN.md)开始：创建自己的 main.cpp，逐步加入网络和协议，
每章核对输出并解释生命周期。配套 example 用于验证，不代替教程正文。

使用 C++20 协程构建异步网络应用。

[项目首页](../README.zh-CN.md) · [English](README.md) · [API 索引](reference/README.zh-CN.md)

## 从这里开始

按[第一章](tutorial/01-build.zh-CN.md)创建自己的 main.cpp 和 xmake.lua，
再沿主线加入异步任务、TCP 和 HTTP。每章有构建步骤、预期结果和可修改的练习。

[构建](tutorial/01-build.zh-CN.md) → [任务](tutorial/02-tasks-timers.zh-CN.md) → [TCP](tutorial/03-tcp.zh-CN.md) → [HTTP](tutorial/04-http.zh-CN.md) → [线程](tutorial/05-execution.zh-CN.md) → [关闭](tutorial/06-shutdown.zh-CN.md)

## 按层选择模块

只链接应用需要的模块，核心层没有第三方依赖。

| 层级 | 使用指南 | 可运行示例 |
|---|---|---|
| **运行时** | [任务](reference/task.md) · [事件循环](reference/io-context.md) · [工作线程](reference/runtime.md) | [任务](../examples/tutorial_tasks.cpp) · [工作线程](../examples/tutorial_workers.cpp) |
| **网络核心** | [TCP](tutorial/03-tcp.zh-CN.md) · [UDP](tutorial/07-udp.zh-CN.md) | [TCP](../examples/tutorial_tcp.cpp) · [UDP](../examples/tutorial_udp.cpp) |
| **基础协议扩展** | [TLS](tutorial/08-dns-tls.zh-CN.md) · [DNS](tutorial/08-dns-tls.zh-CN.md) · [QUIC](tutorial/11-quic-http3.zh-CN.md) | [TLS](../examples/tutorial_tls.cpp) · [DNS](../examples/tutorial_dns.cpp) · [QUIC](../examples/tutorial_quic.cpp) |
| **应用协议** | [HTTP/1.1](tutorial/04-http.zh-CN.md) · [HTTP/2](tutorial/10-http2.zh-CN.md) · [HTTP/3](tutorial/11-quic-http3.zh-CN.md) · [WebSocket](tutorial/09-websocket.zh-CN.md) | [HTTP](../examples/tutorial_http.cpp) · [HTTP/2](../examples/tutorial_http2.cpp) · [HTTP/3](../examples/tutorial_http3.cpp) · [WebSocket](../examples/tutorial_websocket.cpp) |

HTTP/1.1 和 WebSocket 默认启用；TLS、DNS、QUIC、HTTP/2、HTTP/3 按需开启，
详见[构建选项](tutorial/01-build.zh-CN.md)。TLS/QUIC 示例使用 `tests/certs` 中公开的测试专用证书。

## 了解设计

- [架构](architecture.md)：分层、事件循环、所有权与调度。
- [API 参考](reference/README.zh-CN.md)：签名、错误处理与并发约定。
- [教程](tutorial/README.zh-CN.md)：从自己的文件开始，逐步构建应用并核对结果。
- [性能](performance.md)：本地 Linux 完整数据、取舍与复现方法。
- [平台支持](reference/platform.md)：各平台后端与支持的配置。

TCP、UDP 与 QUIC 支持数字 IPv4/IPv6 地址，DNS 可解析 A 和 AAAA。
HTTP/1 支持请求头准入、逐请求 body 限额和有界流式响应；HTTP/2、HTTP/3 的 body 仍采用缓冲方式。
各协议的具体边界见对应参考页。
