<!-- source-hash: 01f490c195b2 -->
# PACE 文档

[English](README.md) · [项目主页](../README.zh.md)

PACE 是一个 C++20 协程网络库：由反应器和任务构成的运行时，TCP 与 UDP，TLS、DNS、QUIC、HTTP/1.1、HTTP/2、HTTP/3
以及 WebSocket。文档按你想做的事情来组织。

## 我想……

| | 从这里开始 |
|:--|:--|
| ……五分钟内跑起一个程序 | 先[安装并编写第一个程序](tutorial/01-install-and-first-program.zh.md)，再写[一个 HTTP 客户端](tutorial/02-http-client.zh.md) |
| ……系统地学习 PACE | 按顺序阅读[教程](tutorial/README.zh.md)：HTTP、任务、TCP、UDP、结构化并发、工作线程、关闭，然后是各个协议 |
| ……解决一个具体问题 | 查看[指南](guides/README.zh.md)：HTTP 配置、协议调优 |
| ……查找某个类型或函数 | 查阅[参考](reference/README.zh.md)，每个模块一节 |
| ……理解它如何工作、为什么这样设计 | 阅读[设计说明](explanation/README.zh.md)：架构、执行模型、所有权、API 设计 |
| ……了解它有多快 | 查看[性能](performance/README.zh.md)，附测量方法和原始数据 |
| ……从旧版本升级 | 查看[迁移](migration/README.zh.md) |
| ……参与开发 | 查看[参与开发](contributing/README.zh.md)：构建、测试、代码风格、文档 |

## 模块

| 模块 | 头文件 | 参考 | 教程 |
|:--|:--|:--|:--|
| 运行时 | `<pace/runtime.hpp>` | [运行时](reference/runtime/README.zh.md) | [任务与定时器](tutorial/04-tasks-and-timers.zh.md)、[结构化并发](tutorial/07-structured-concurrency.zh.md)、[工作线程](tutorial/08-workers-and-threads.zh.md)、[关闭](tutorial/09-graceful-shutdown.zh.md) |
| 网络 | `<pace/net.hpp>` | [TCP 与 UDP](reference/net/README.zh.md) | [TCP](tutorial/05-tcp.zh.md)、[UDP](tutorial/06-udp.zh.md) |
| TLS、DNS | `<pace/tls.hpp>`、`<pace/dns.hpp>` | [TLS](reference/tls/README.zh.md)、[DNS](reference/dns/README.zh.md) | [TLS 与 DNS](tutorial/10-tls-and-dns.zh.md) |
| QUIC | `<pace/quic.hpp>` | [QUIC](reference/quic/README.zh.md) | [QUIC 与 HTTP/3](tutorial/13-quic-and-http3.zh.md) |
| HTTP | `<pace/http.hpp>` | [HTTP](reference/http/README.zh.md) | [客户端](tutorial/02-http-client.zh.md)、[服务端](tutorial/03-http-server.zh.md) |
| HTTP/1.1、HTTP/2、HTTP/3 | `<pace/http1.hpp>`、`<pace/http2.hpp>`、`<pace/http3.hpp>` | [HTTP/1.1](reference/http1/README.zh.md)、[HTTP/2](reference/http2/README.zh.md)、[HTTP/3](reference/http3/README.zh.md) | [HTTP/2](tutorial/12-http2.zh.md)、[HTTP/3](tutorial/13-quic-and-http3.zh.md) |
| WebSocket | `<pace/websocket.hpp>` | [WebSocket](reference/websocket/README.zh.md) | [WebSocket](tutorial/11-websocket.zh.md) |
| 网关 | `<pace/gateway.hpp>` | [网关](reference/gateway/README.zh.md) | [静态文件与代理](tutorial/14-gateway.zh.md) |

[构建选项](reference/build-options.zh.md) · [平台支持](reference/platform-support.zh.md) ·
[错误](reference/errors.zh.md)
