# 跟着写出一个 PACE 应用

先从 [0.3.0 快速入门](00-quick-start.zh-CN.md)开始：安装包、几行启动 HTTP 服务，再逐步加入异步等待和 URL 客户端。

[English](README.md) · [API 中文索引](../reference/README.zh-CN.md)

这里按“创建自己的文件 → 构建运行 → 核对结果 → 增加一个功能”的顺序学习。
每章解释代码为什么这样组织、等待期间谁持有数据、哪里处理错误。
完整源码作为配套练习，不再代替正文讲解。教程对应当前 checkout；
`pace::run`、router 和持有 listener 的 HTTP 入口是 v0.2.0 之后新增的接口。

## 先完成主线

| 章节 | 这一章会写出什么、理解什么 |
|---|---|
| [1. 自己的工程](01-build.zh-CN.md) | 创建 main.cpp、xmake.lua，选择实际需要的库模块 |
| [2. 任务与定时器](02-tasks-timers.zh-CN.md) | 等待后返回结果，理解惰性、所有权、异常和入口自动停止 |
| [3. TCP](03-tcp.zh-CN.md) | 分别写服务器和客户端，正确拼出完整消息 |
| [4. HTTP](04-http.zh-CN.md) | 从健康检查扩展到路由、POST、异步处理器、客户端和 HTTPS |
| [5. 执行方式](05-execution.zh-CN.md) | 选择 worker 和应用线程，明确任务实际在哪里执行 |
| [6. 关闭](06-shutdown.zh-CN.md) | 观察取消，理解停止接收、drain、join、销毁的先后顺序 |

第一次使用从第一章开始，不必先读完整 example 再猜怎么改。
`my_application` 等命令在自己的练习工程运行；`tutorial_*` 是 PACE 仓库根目录的
配套目标，需要 `--examples=y`。默认配套程序会自行结束，带 `serve` 的服务器则持续运行。

## 再按需要增加协议

| 章节 | 相比已有代码增加的能力 | 配套目标 |
|---|---|---|
| [7. UDP](07-udp.zh-CN.md) | 数据报边界、来源地址、截断检查 | `tutorial_udp` |
| [8. DNS 与 TLS](08-dns-tls.zh-CN.md) | 地址解析、保留域名身份、验证握手与关闭 | `tutorial_dns`、`tutorial_tls` |
| [9. WebSocket](09-websocket.zh-CN.md) | 从 TCP 升级到完整消息与关闭握手 | `tutorial_websocket` |
| [10. HTTP/2](10-http2.zh-CN.md) | 批量请求、多路复用、有界异步 handler | `tutorial_http2` |
| [11. QUIC 与 HTTP/3](11-quic-http3.zh-CN.md) | 流方向、ALPN，以及协议连接的所有权转移 | `tutorial_quic`、`tutorial_http3` |

每章开头给出要启用的选项。TLS/QUIC 配套练习使用公开测试证书和本地连接，
不能把测试私钥拿去部署。DNS 默认解析数字回环地址，传入真实域名才需要外部网络。
QUIC/HTTP3 当前要求 POSIX；详见[构建配置](01-build.zh-CN.md)和[平台范围](../reference/platform.md)。

写通一个实验后，再查链接中的 API 契约，确认默认值、边界与生命周期。
简化接口的设计取舍见[易用性说明](../ergonomics.md)。
