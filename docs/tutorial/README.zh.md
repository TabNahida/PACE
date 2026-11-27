<!-- source-hash: 743952b48790 -->
# 教程

[English](README.md) · [文档首页](../README.zh.md) · [API 参考](../reference/README.zh.md)

本教程是按顺序阅读的一条学习路线。你先建立自己的项目和一个 HTTP 服务器，在不使用任何协程的情况下向它发请求，然后依次学习任务、socket、并发和关闭，最后进入可选协议。每一章都给出完整的文件、构建命令和预期输出，并以一个能运行的程序结束。

## 学习路线

| 章节 | 构建什么、学到什么 | 配套程序 |
|:--|:--|:--|
| [1. 安装 PACE，构建第一个程序](01-install-and-first-program.zh.md) | 安装 xmake 和编译器，创建 `main.cpp` 与 `xmake.lua`，依赖一个模块，构建并运行；命名模块 | `tutorial_tasks` |
| [2. 发送 HTTP 请求](02-http-client.zh.md) | 直接调用 `get`、`post` 和 `fetch`，发送 body、设置超时、HTTPS；何时改用 `async_*` | `tutorial_http_client` |
| [3. 构建 HTTP 服务器](03-http-server.zh.md) | 路由、构造响应、会等待的处理函数、在处理函数中等待另一个请求 | `tutorial_http` |
| [4. 任务与定时器](04-tasks-and-timers.zh.md) | 从协程返回结果；所有权、异常以及 `pace::run` 如何结束 | `tutorial_tasks` |
| [5. TCP](05-tcp.zh.md) | `tcp::exchange`、回显服务器、分帧 | `tutorial_tcp` |
| [6. UDP](06-udp.zh.md) | 请求与回复、只发送、回显服务器 | `tutorial_udp` |
| [7. 结构化并发](07-structured-concurrency.zh.md) | 任务组、共享截止时间、局部取消 | `tutorial_structured` |
| [8. 工作线程与线程](08-workers-and-threads.zh.md) | 两个事件循环、应用线程、跨线程提交工作 | `tutorial_workers` |
| [9. 有序关闭](09-graceful-shutdown.zh.md) | 取消、停止接受连接、排空、按顺序销毁 | `tutorial_shutdown` |
| [10. TLS 与 DNS](10-tls-and-dns.zh.md) | 解析名字、校验 TLS 对端、区分地址与身份 | `tutorial_dns`、`tutorial_tls` |
| [11. WebSocket](11-websocket.zh.md) | 升级 TCP 连接、交换消息、关闭 | `tutorial_websocket` |
| [12. HTTP/2](12-http2.zh.md) | 一个 GET，然后是多路复用的批量请求 | `tutorial_http2` |
| [13. QUIC 与 HTTP/3](13-quic-and-http3.zh.md) | QUIC 流与 FIN，然后在其上运行 HTTP/3 | `tutorial_quic`、`tutorial_http3` |
| [14. 静态文件与反向代理](14-gateway.zh.md) | 挂载目录、代理 API、缓存、多工作线程 | |

除了第 2 章的 HTTPS 一节，第 1 到 9 章只用到默认构建包含的内容。你自己的程序放在第 1 章的练习项目中，并在那里运行。配套程序是各章练习的完整、可自检的版本，位于 PACE 的 `examples/` 目录；在 PACE 源码目录中以 `--examples=y` 配置后构建，再用 `xmake run <名称>` 运行。它们会自行结束；保持监听的服务器（`tutorial_http serve`、`tutorial_tcp serve`、`tutorial_websocket serve`）用 Ctrl-C 停止。

## 可选模块

第 10 到 13 章需要默认关闭的模块。配置时加上它们的选项，你的项目和用于运行配套程序的 PACE 源码目录都要加；xmake 第一次会下载第三方库。

| 章节 | 选项 | 下载 |
|:--|:--|:--|
| 10. TLS 与 DNS | `--tls=y --dns=y` | OpenSSL、c-ares |
| 11. WebSocket | `--websocket=y`（默认开启） | |
| 12. HTTP/2 | `--http2=y`，HTTPS 另加 `--tls=y` | nghttp2 |
| 13. QUIC 与 HTTP/3 | 只用 QUIC 时 `--quic=y`，两者都用时 `--http3=y` | OpenSSL、ngtcp2、nghttp3 |

QUIC 和 HTTP/3 需要 POSIX 平台。TLS 和 QUIC 练习使用 `tests/certs` 中公开的测试证书，它们仅供本地练习。DNS 默认解析数字形式的 `127.0.0.1`，只有解析真实主机名时才需要外部网络。具体配置见[构建选项](../reference/build-options.zh.md)和[平台支持](../reference/platform-support.zh.md)。

## 学完教程之后

某一章的练习跑通之后，它对应的参考页给出默认值、限制和生命周期规则。[实用指南](../guides/README.zh.md)收录面向任务的配方，例如 [HTTP 实用配方](../guides/http-recipes.zh.md)和[协议调优](../guides/protocol-tuning.zh.md)。想了解简短的客户端、入口和路由写法背后的考虑，请阅读 [API 设计](../explanation/api-design.zh.md)。
