# 从运行程序开始学习 PACE

[English](README.md) · [API 中文索引](../reference/README.zh-CN.md)

## 1. 先打印 Hello

在仓库根目录运行：

```sh
xmake f -m release -y
xmake -y tutorial_tasks
xmake run tutorial_tasks
```

程序打印 `Hello, PACE` 后退出。需要安装工具链或添加自己的应用时，阅读
[构建与集成](01-build.md)；接着看[任务与定时器](02-tasks-timers.md)和
[完整源码](../../examples/tutorial_tasks.cpp)，理解 `co_await`、惰性任务和数据所有权。
任务结束不会自动停止事件循环，示例会显式调用 `io.stop()`。

## 2. 选择要用的协议

先运行 `xmake -y` 构建示例。下表中的默认命令都是完整程序，运行后会自行结束。

| 目标 | 命令 | 指南 / 源码 |
|---|---|---|
| TCP 回显 | `xmake run tutorial_tcp` | [指南](03-tcp.md) · [源码](../../examples/tutorial_tcp.cpp) |
| DNS 解析 | `xmake run tutorial_dns` | [API](../reference/dns.md) · [source](../../examples/tutorial_dns.cpp) |
| QUIC 多路流 | `xmake run tutorial_quic` | [API](../reference/quic.md) · [source](../../examples/tutorial_quic.cpp) |
| HTTP/3 | `xmake run tutorial_http3` | [API](../reference/http3.md) · [source](../../examples/tutorial_http3.cpp) |
| UDP 数据报 | `xmake run tutorial_udp` | [指南](../reference/udp.md) · [源码](../../examples/tutorial_udp.cpp) |
| HTTP 服务端 | `xmake run tutorial_http` | [指南](04-http.md) · [源码](../../examples/tutorial_http.cpp) |
| HTTP 客户端 | `xmake run tutorial_http_client` | [指南](04-http.md) · [源码](../../examples/tutorial_http_client.cpp) |
| TLS 加密连接 | `xmake run tutorial_tls` | [指南](../reference/tls.md) · [源码](../../examples/tutorial_tls.cpp) |
| HTTP/2 请求 | `xmake run tutorial_http2` | [指南](../reference/http2.md) · [源码](../../examples/tutorial_http2.cpp) |
| WebSocket 消息 | `xmake run tutorial_websocket` | [指南](../reference/websocket.md) · [源码](../../examples/tutorial_websocket.cpp) |

TLS、DNS、QUIC、HTTP/2 和 HTTP/3 默认关闭，运行相应示例前需要启用并重新构建：

```sh
xmake f -m release --tls=y --dns=y --quic=y --http2=y --http3=y -y
xmake -y
```

TLS 示例以仓库根目录为工作目录，加载 `tests/certs` 下的公开测试证书。
这些证书和密钥仅供测试，不能部署。TCP、HTTP 示例加上 `serve` 可持续提供服务，
用 Ctrl-C 停止。HTTP 客户端最短调用是
`co_await pace::http::get(io, endpoint, "/health")`，
[HTTP 指南](04-http.md)还介绍 HTTPS 和连接复用。

## 3. 再选择线程与关闭方式

```sh
xmake run tutorial_workers
xmake run tutorial_workers background
xmake run tutorial_shutdown
```

默认 worker 示例用 `runtime(2).run_on_current_thread()`，调用线程运行
context 0，另一个后台线程运行 context 1。`background` 模式使用 `run()`，
两个 context 都在后台线程运行，调用线程等待。也可直接运行 `io_context`，
不使用 runtime。详见[执行方式](05-execution.md)。

[关闭教程](06-shutdown.md)说明停止接收、等待连接、取消和 join 的顺序。
`stop()` 只是请求停止；必须等待 `run()` 返回或 join 后再销毁执行环境。
示例用于检查行为，性能应通过[专门的基准工具](../../benchmarks/README.md)测量。

## 4. 按需查 API

[中文索引](../reference/README.zh-CN.md)解释常用接口和关键约定，
[英文参考](../reference/README.md)提供签名、默认参数、所有权、线程要求与错误。
先读完整示例，再查所用协议，无需按顺序阅读所有 API。
