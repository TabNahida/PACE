# API 中文索引与关键约定

完整签名、默认参数、异常和边界行为以对应的英文参考页为准。
这些页面覆盖公共头文件中的所有公开类型与成员；`pace::detail` 属于内部实现。

| 功能 | 公开接口 | 参考 |
|---|---|---|
| 协程任务 | `task<T>`、`task<void>`、移动、`release`、右值 `co_await`、promise/awaiter 协议 | [task](task.md) |
| 提交与运行 | `io_context::spawn/post/run/stop/stopped/current` | [io_context](io-context.md) |
| 等待与取消 | `wait_readable/wait_writable/cancel/sleep_for/yield` 及 awaiter 类型 | [io_context](io-context.md) |
| 工作线程 | `runtime(workers)`、`size/context/run/run_on_current_thread/stop` | [runtime](runtime.md) |
| TCP | `tcp::endpoint/listen_options/stream/listener` | [TCP](tcp.md) |
| HTTP | `http::header_field/request/response/limits/handler/serve/serve_connection` | [HTTP](http.md) |
| 平台与兼容 | `native_socket_type/invalid_socket`、旧 TCP 名称、公共头文件 | [平台与兼容](platform.md) |

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

TCP endpoint 仅支持数字 IPv4。每个 socket 最多并行一个读和一个写，
不能并行两个读、两个写或移动有未完成操作的 socket。
`read_some` 不保证填满缓冲区；非空缓冲区返回零表示 EOF。
`write_all` 完成表示数据已交给本机网络栈，不代表对端已经处理。

`close()` 取消等待并关闭句柄，但操作恢复前仍须保留 socket 对象。
`close_gracefully()` 要求没有其他未完成操作，默认在发送半关闭后最多排空
100 ms 或 64 KiB 的对端输入，然后关闭；这不是请求超时 API。

HTTP 支持 HTTP/1.0、HTTP/1.1、Content-Length 和持久连接。
默认请求头上限 16 KiB、请求体上限 1 MiB、每连接最多 1000 个请求。
响应的 Content-Length、Connection、Content-Type 等由库管理。
暂不支持 TLS、chunked、HTTP/2、WebSocket、CONNECT、流式 body、请求截止时间，
也没有通用 HTTP 客户端。各错误状态和解析细节见 [HTTP 参考](http.md)。

## 停止与资源释放

`stop()` 是永久的协作式停止请求。等待中的 I/O 和 timer 通过
`std::system_error` 报告 `std::errc::operation_canceled`，已排队工作继续得到清理机会。
新提交会被拒绝。无法返回或挂起的代码会阻止停止完成。

要让已有响应尽量完成，应先停止接收新连接、等待应用跟踪的连接退出，
再到达宽限期后停止 context。最后等 run 返回或 join 运行线程，
销毁 socket/listener，再销毁 context。参见[关闭教程](../tutorial/06-shutdown.md)。
