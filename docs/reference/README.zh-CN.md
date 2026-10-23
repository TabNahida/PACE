# API 中文索引与关键约定

完整签名、默认参数、异常和边界行为以对应的英文参考页为准。
这些页面覆盖公共头文件中的所有公开类型与成员；`pace::detail` 属于内部实现。

| 功能 | 公开接口 | 参考 |
|---|---|---|
| DNS 扩展 | `dns::resolver`、A/AAAA/CNAME、超时、取消 | [DNS](dns.md) |
| QUIC 扩展 | `quic::context/listener/connection/stream`、TLS 1.3、多路流 | [QUIC](quic.md) |
| HTTP/3 应用协议 | `http3::exchange/serve_connection/serve`、nghttp3/QPACK | [HTTP/3](http3.md) |
| 协程任务 | `task<T>`、`task<void>`、移动、`release`、右值 `co_await`、promise/awaiter 协议 | [task](task.md) |
| 提交与运行 | `io_context::spawn/post/run/stop/stopped/current` | [io_context](io-context.md) |
| 等待与取消 | `wait_readable/wait_writable/cancel/sleep_for/yield` 及 awaiter 类型 | [io_context](io-context.md) |
| 工作线程 | `runtime(workers)`、`size/context/run/run_on_current_thread/stop` | [runtime](runtime.md) |
| TCP | `tcp::endpoint/listen_options/stream/listener` | [TCP](tcp.md) |
| UDP | `udp::endpoint/datagram/socket`、`send_to/receive_from` | [UDP](udp.md) |
| HTTP/HTTPS | 消息与限额、`handler/serve/serve_connection`、`client/secure_client`、`get/fetch`、协议异常 | [HTTP](http.md) |
| TLS | `tls::context/stream`、证书验证、握手、ALPN、加密读写与关闭 | [TLS](tls.md) |
| HTTP/2 | `http2::limits/exchange/serve/serve_connection`，有界批量请求 | [HTTP/2](http2.md) |
| WebSocket | 握手、消息、控制帧与可选加密传输 | [WebSocket](websocket.md) |
| 平台与兼容 | `native_socket_type/invalid_socket`、旧 TCP 名称、公共头文件 | [平台与兼容](platform.md) |

先运行程序可直接前往[按协议选择示例](../tutorial/README.zh-CN.md)。TLS 需要
`--tls=y`，HTTP/2 需要 `--http2=y`；使用 TLS 承载 HTTP/2 时两个开关都要启用。

## 协程与数据所有权

`task` 是惰性、只能消费一次的移动类型。调用协程函数会创建协程状态，
但函数体到 `co_await` 或 `spawn` 后才执行。具名任务要写成
`co_await std::move(task)`；空任务或已被消费的任务不能再次等待。
子任务异常从 `co_await` 抛出，根任务未处理异常会让 context 停止并从 `run()` 抛出。

按值传入的字符串等对象可由协程帧持有；引用、指针、`span`、`string_view`
不会延长底层对象寿命。尤其不要让临时捕获型协程 lambda 的闭包先于协程销毁。
推荐具名协程函数，并按值传递需要跨挂起点保存的数据。

## 线程和执行模式

每个 context 同时只由一个线程运行，任务不会自动迁移。`spawn/post/stop`
可以跨线程调用；socket I/O、显式取消、关闭，以及 timer/yield 等待应在所属
context 上执行。运行前构造、运行线程结束后清理是支持的。

`runtime(N).run()` 创建 N 个后台工作线程，调用线程等待；
`runtime(N).run_on_current_thread()` 让 context 0 在调用线程运行，另建 N−1 个线程。
N 是 context 总数，必须大于零；N=1 的调用线程模式不创建 runtime 工作线程。
两种调用都阻塞到停止和清理结束。runtime 和 context 均不可重启。

同步阻塞调用仍会阻塞当前 reactor。库没有内置可等待的阻塞任务执行器；
可用应用自己的线程执行，然后通过 `post` 发送拥有数据所有权的结果。
退出时必须先 join 这些线程，避免它们访问已经销毁的 context。

## TCP 和 HTTP 边界

TCP endpoint 支持数字 IPv4/IPv6、地址族与数字 scope_id；IPv6 监听默认仅 IPv6，可显式选择双栈。每个 socket 最多并行一个读和一个写，
不能并行两个读、两个写或移动有未完成操作的 socket。
`read_some` 不保证填满缓冲区；非空缓冲区返回零表示 EOF。
`write_all` 完成表示数据已交给本机网络栈，不代表对端已经处理。

UDP 保留数据报边界，零长度数据报有效；截断通过 `datagram::truncated` 报告。
TLS 当前只允许**一个异步操作**同时进行，读写也不能重叠；TCP 的双向并发
规则不适用于 TLS。所有协议都要求借用缓冲区活到异步操作完成。

`close()` 取消等待并关闭句柄，但操作恢复前仍须保留 socket 对象。
`tcp::stream::close_gracefully()` 要求没有其他未完成操作，默认在发送半关闭后
最多排空 100 ms 或 64 KiB 的对端输入，然后关闭；这不是请求超时 API。
TLS 的关闭会交换 `close_notify`，WebSocket 的关闭会交换关闭帧；它们的具体
时限、返回值和错误语义分别见 [TLS](tls.zh-CN.md) 和 [WebSocket](websocket.zh-CN.md)。

HTTP 提供 HTTP/1.0、HTTP/1.1 客户端与服务端，支持 Content-Length、chunked、trailer 和持久连接。
默认请求头上限 16 KiB、请求体上限 1 MiB、每连接最多 1000 个请求。
响应的 Content-Length、Connection、Content-Type 等由库管理。
启用 `--tls=y` 后，同样的 `get/fetch/serve` 接口有 HTTPS 重载，默认校验证书链和主机名。
`http::get(io, endpoint, "/health")` 可以完成一次请求；`http::client` 支持顺序复用连接。
HTTP/1.1 服务端收到任何 `Expect`（包括 `100-continue`）都会返回 417。客户端
拒绝自行设置 `Expect`，直接发送完整请求，然后可跳过数量受限的 100/103 等中间
响应，继续读取最终响应；这不包含“先发请求头，等 100 许可后再发送 body”的握手。
HTTP/2 与 WebSocket 使用独立接口；本 HTTP/1.1 接口不提供 CONNECT、流式 body、
请求截止时间、自动 DNS/重定向/重试/解压。各错误状态和解析细节见 [HTTP 参考](http.md)。

## 停止与资源释放

`stop()` 是永久的协作式停止请求。等待中的 I/O 和 timer 通过
`std::system_error` 报告 `std::errc::operation_canceled`，已排队工作继续得到清理机会。
新提交会被拒绝。无法返回或挂起的代码会阻止停止完成。

要让已有响应尽量完成，应先停止接收新连接、等待应用跟踪的连接退出，
再到达宽限期后停止 context。最后等 run 返回或 join 运行线程，
销毁 socket/listener，再销毁 context。参见[关闭教程](../tutorial/06-shutdown.md)。
