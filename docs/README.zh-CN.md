# PACE 中文文档

[English](README.md) · [API 中文索引](reference/README.zh-CN.md)

## 先运行一个程序

准备 xmake 2.9.8+ 和支持 C++20 协程的编译器，在仓库根目录运行：

```sh
xmake f -m release -y
xmake -y tutorial_tasks
xmake run tutorial_tasks
```

程序打印 `Hello, PACE` 后退出。核心流程是
`io.spawn(hello(io)); io.run();`：调用线程执行任务，任务显式调用
`io.stop()` 后退出。继续阅读[构建与应用集成](tutorial/01-build.md)，
或直接看[完整源码](../examples/tutorial_tasks.cpp)。

## 按要做的事选择路径

先用 `xmake -y` 构建默认示例，再选择：

| 我想要…… | 从这里开始 | 完整源码 |
|---|---|---|
| 组合协程任务与定时器 | [任务教程](tutorial/02-tasks-timers.md) | [tutorial_tasks.cpp](../examples/tutorial_tasks.cpp) |
| 建立 TCP 客户端或服务 | [TCP 教程](tutorial/03-tcp.md) | [tutorial_tcp.cpp](../examples/tutorial_tcp.cpp) |
| 收发 UDP 数据报 | [UDP 参考](reference/udp.md) | [tutorial_udp.cpp](../examples/tutorial_udp.cpp) |
| 调用或提供 HTTP 接口 | [HTTP 教程](tutorial/04-http.md) | [客户端](../examples/tutorial_http_client.cpp) · [服务端](../examples/tutorial_http.cpp) |
| 加密连接或使用 HTTPS | [TLS](reference/tls.md) · [HTTPS](reference/http.md) | [tutorial_tls.cpp](../examples/tutorial_tls.cpp) |
| 交换 HTTP/2 请求 | [HTTP/2 参考](reference/http2.md) | [tutorial_http2.cpp](../examples/tutorial_http2.cpp) |
| 收发 WebSocket 消息 | [WebSocket 参考](reference/websocket.md) | [tutorial_websocket.cpp](../examples/tutorial_websocket.cpp) |
| 设置工作线程 | [执行方式](tutorial/05-execution.md) | [tutorial_workers.cpp](../examples/tutorial_workers.cpp) |
| 停止服务并释放资源 | [关闭流程](tutorial/06-shutdown.md) | [tutorial_shutdown.cpp](../examples/tutorial_shutdown.cpp) |

TLS 和 HTTP/2 需要先启用可选开关：
`xmake f -m release --tls=y --http2=y -y`，再运行 `xmake -y`。
默认教程都会自行结束。TLS 示例使用 `tests/certs` 下公开的**测试专用**证书；
真实服务必须使用自己的证书和密钥。

## 再查 API 细节

[API 中文索引](reference/README.zh-CN.md)说明关键约定，
[英文 API 参考](reference/README.md)提供签名、所有权、线程限制和错误。
优先阅读所选协议，再查 [task](reference/task.md)、
[io_context](reference/io-context.md) 和 [runtime](reference/runtime.md)。

[架构](architecture.md)解释事件循环设计；
[完整性能与调优](performance.md)包含所有后端、退化项、吞吐、延迟、
进程内存的解释和复现方法；[中文教程](tutorial/README.zh-CN.md)提供运行命令。

PACE 仍在开发中。目前支持数字 IPv4，尚无 DNS、IPv6、异步文件 I/O、
HTTP 流式 body 或单操作截止时间。各协议均有明确边界，使用前请阅读对应参考页。
