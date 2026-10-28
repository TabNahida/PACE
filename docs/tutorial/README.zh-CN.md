# 用 PACE 逐步构建应用

[English](README.md) · [API 参考](../reference/README.zh-CN.md)

从[一次调用的客户端](00-quick-start.zh-CN.md)开始：先写直接 HTTP GET，准备本地服务，
构建运行并观察响应，再按需尝试 TCP 或 UDP。需要组合异步操作或编写服务端时，再学习协程。

教程需要 PACE 0.4.0 或更新版本，包含直接客户端和 HTTP 的 `async_` 命名；
下文的源码构建方式适用于 v0.4.0 checkout。每章包含完整文件、构建步骤、预期结果和可修改的练习；配套程序用于独立验证。

## 先完成主线

| 章节 | 这一章会写出什么、理解什么 |
|---|---|
| [1. 自己的工程](01-build.zh-CN.md) | 创建 main.cpp、xmake.lua，选择实际需要的库模块 |
| [4. HTTP](04-http.zh-CN.md) | 先直接 GET/POST/fetch，再讲路由、异步处理与 HTTPS |
| [3. TCP](03-tcp.zh-CN.md) | 先用 `tcp::exchange`，再编写回显服务端、理解分帧 |
| [7. UDP](07-udp.zh-CN.md) | 直接收发、只发送，再编写异步回显服务端 |
| [2. 任务与定时器](02-tasks-timers.zh-CN.md) | 等待后返回结果，理解惰性、所有权、异常和入口自动停止 |
| [12. 结构化并发](12-structured-concurrency.zh-CN.md) | 并发子任务、局部取消与共同 deadline |
| [5. 执行方式](05-execution.zh-CN.md) | 选择 worker 和应用线程，明确任务实际在哪里执行 |
| [6. 关闭](06-shutdown.zh-CN.md) | 观察取消，理解停止接收、drain、join、销毁的先后顺序 |

先完成快速入门，再按需要选择协议章节。
`my_application` 等命令在自己的练习工程运行；`tutorial_*` 是 PACE 仓库根目录的
配套目标，需要 `--examples=y`。默认配套程序会自行结束，带 `serve` 的服务器则持续运行。

## 再按需要增加协议

| 章节 | 相比已有代码增加的能力 | 配套目标 |
|---|---|---|
| [8. DNS 与 TLS](08-dns-tls.zh-CN.md) | 地址解析、保留域名身份、验证握手与关闭 | `tutorial_dns`、`tutorial_tls` |
| [9. WebSocket](09-websocket.zh-CN.md) | 从 TCP 升级到完整消息与关闭握手 | `tutorial_websocket` |
| [10. HTTP/2](10-http2.zh-CN.md) | 先完成单次 GET，再进入批量多路复用 | `tutorial_http2` |
| [11. QUIC 与 HTTP/3](11-quic-http3.zh-CN.md) | 流方向、ALPN，以及协议连接的所有权转移 | `tutorial_quic`、`tutorial_http3` |

每章开头给出要启用的选项。TLS/QUIC 配套练习使用公开测试证书和本地连接，
不能把测试私钥拿去部署。DNS 默认解析数字回环地址，传入真实域名才需要外部网络。
QUIC/HTTP3 当前要求 POSIX；详见[构建配置](01-build.zh-CN.md)和[平台范围](../reference/platform.md)。

写通一个实验后，再查链接中的 API 契约，确认默认值、边界与生命周期。
简化接口的设计取舍见[易用性说明](../ergonomics.md)。

完整接口约定见[组件便捷接口](../reference/component-conveniences.zh-CN.md)；缓冲区复用、期限和并发等细节见[协议调优](../reference/protocol-tuning.zh-CN.md)。

[直接客户端与 API 迁移](../reference/direct-clients.zh-CN.md)
