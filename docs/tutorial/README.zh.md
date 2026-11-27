# 用 PACE 逐步构建应用

[English](README.md) · [API 参考](../reference/README.zh.md)

从[一次调用的客户端](02-http-client.zh.md)开始：先写直接 HTTP GET，准备本地服务，
构建运行并观察响应，再按需尝试 TCP 或 UDP。需要组合异步操作或编写服务端时，再学习协程。

教程需要 PACE 0.4.0 或更新版本，包含直接客户端和 HTTP 的 `async_` 命名；
下文的源码构建方式适用于 v0.4.0 checkout。每章包含完整文件、构建步骤、预期结果和可修改的练习；配套程序用于独立验证。

## 先完成主线

| 章节 | 这一章会写出什么、理解什么 |
|---|---|
| [1. 自己的工程](01-install-and-first-program.zh.md) | 创建 main.cpp、xmake.lua，选择实际需要的库模块 |
| [4. HTTP](03-http-server.zh.md) | 先直接 GET/POST/fetch，再讲路由、异步处理与 HTTPS |
| [3. TCP](05-tcp.zh.md) | 先用 `tcp::exchange`，再编写回显服务端、理解分帧 |
| [7. UDP](06-udp.zh.md) | 直接收发、只发送，再编写异步回显服务端 |
| [2. 任务与定时器](04-tasks-and-timers.zh.md) | 等待后返回结果，理解惰性、所有权、异常和入口自动停止 |
| [12. 结构化并发](07-structured-concurrency.zh.md) | 并发子任务、局部取消与共同 deadline |
| [5. 执行方式](08-workers-and-threads.zh.md) | 选择 worker 和应用线程，明确任务实际在哪里执行 |
| [6. 关闭](09-graceful-shutdown.zh.md) | 观察取消，理解停止接收、drain、join、销毁的先后顺序 |

先完成快速入门，再按需要选择协议章节。
`my_application` 等命令在自己的练习工程运行；`tutorial_*` 是 PACE 仓库根目录的
配套目标，需要 `--examples=y`。默认配套程序会自行结束，带 `serve` 的服务器则持续运行。

## 再按需要增加协议

| 章节 | 相比已有代码增加的能力 | 配套目标 |
|---|---|---|
| [8. DNS 与 TLS](10-tls-and-dns.zh.md) | 地址解析、保留域名身份、验证握手与关闭 | `tutorial_dns`、`tutorial_tls` |
| [9. WebSocket](11-websocket.zh.md) | 从 TCP 升级到完整消息与关闭握手 | `tutorial_websocket` |
| [10. HTTP/2](12-http2.zh.md) | 先完成单次 GET，再进入批量多路复用 | `tutorial_http2` |
| [11. QUIC 与 HTTP/3](13-quic-and-http3.zh.md) | 流方向、ALPN，以及协议连接的所有权转移 | `tutorial_quic`、`tutorial_http3` |

每章开头给出要启用的选项。TLS/QUIC 配套练习使用公开测试证书和本地连接，
不能把测试私钥拿去部署。DNS 默认解析数字回环地址，传入真实域名才需要外部网络。
QUIC/HTTP3 当前要求 POSIX；详见[构建配置](01-install-and-first-program.zh.md)和[平台范围](../reference/platform-support.md)。

写通一个实验后，再查链接中的 API 契约，确认默认值、边界与生命周期。
简化接口的设计取舍见[易用性说明](../explanation/api-design.md)。

完整接口约定见[组件便捷接口](../reference/component-conveniences.zh.md)；缓冲区复用、期限和并发等细节见[协议调优](../guides/protocol-tuning.zh.md)。

[直接客户端与 API 迁移](../reference/direct-clients.zh.md)

[13. 静态文件、缓存与反向代理](14-gateway.zh.md) — 从一行挂载开始搭建网关（开发分支）。
