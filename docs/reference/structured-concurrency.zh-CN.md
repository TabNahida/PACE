# 结构化并发、局部取消与统一 deadline

[English / 完整签名与约定](structured-concurrency.md) · [逐步教程](../tutorial/12-structured-concurrency.zh-CN.md)

包含 `<pace/task_group.hpp>`，链接 `pace_runtime`，无需第三方依赖。这些接口需要 PACE 0.4.0 或更新版本。

## 作用域与所有权

`with_task_group(factory, operation_options={})` 返回惰性 `task<T>`。工厂接收 `task_group&`，
返回 `task<T>`，自身由边界持有。任务组不能复制、移动或自行构造，避免析构时遗漏活动子任务。

- `group.spawn(factory/task)` 返回只能消费一次的 `join_handle<T>`，`co_await handle` 获取结果。
  工厂可以无参数或接收 `io_context&`，值捕获由库持有。已经创建的 task 中的引用仍然是借用。
- 丢弃 handle 不会 detach，也不会忽略异常；整个组仍负责等待子任务。
- 主体正常结束会等待子任务；主体或子任务首次未处理异常会取消兄弟任务，全部收尾后再传播首个异常。
  后续异常不覆盖首个，也不聚合。希望局部失败不影响兄弟任务时，在子任务内部捕获它。
- `group.own(value)` 返回组内持有对象的 `T&`，保证所有子任务结束后、边界返回或抛出前才销毁。
- `co_await group.join()` 封闭组并等待，观察局部取消，禁止重叠 join 和后续 spawn/own；外层边界始终排空子任务。
- `group.cancel()` 请求主体及后代取消；`group.token()` 取得观察令牌。取消请求不代替 join。

任务组是同一 reactor 上的协作并发，不是 CPU 线程池。可借用整个边界调用外部的调用方数据；
不能借用主体返回或抛异常时就销毁的主体局部变量，请用值捕获或 `group.own`。
带捕获的协程 lambda 直接传给 spawn，不能立即调用临时 lambda 再传 task。
返回值必须拥有数据，不能返回指向组内数据的 view/reference。
`io.spawn` 仍是显式的非结构化根任务，不继承调用方 token 或 join 所有权。

## 局部取消

`cancellation_source` 可复制，`token()` 返回观察令牌。
`request_cancellation()` 可从任意线程调用，幂等且首个原因生效，报告 `operation_canceled`；
`request_timeout()` 报告 `timed_out`。`token.error()`、`stop_requested()`、`throw_if_cancelled()`
也观察 deadline 是否过期。`source.link(parent)` 单向转发取消，在并发使用前配置；deadline 在操作边界合并。

`with_cancellation(token, work)`、`with_options(work, options)` 接收 task 或工厂并持有到完成。
普通 `co_await task` 向下传递 token；内层边界可以缩短预算，但不会反向取消外层。
`co_await this_task::cancellation()` 取得当前 token，不抛出，适合清理时检查状态。

其他线程只标记取消并唤醒 poller，实际注销和恢复在所属 reactor 完成。局部读取消不会取消同一
描述符的独立写；TCP/UDP 原始 socket 保持打开，但部分数据可能已经读写，取消不是回滚。
HTTP/1 持久客户端失败会关闭连接；HTTP/2、3 批处理拥有整个连接，取消会结束该批处理及连接。
QUIC 只解除相应事件等待，其他流不受影响；DNS 取消后 resolver 可以继续使用。
已就绪操作在恢复时观察取消，若这时已请求取消，取消优先于结果。
不要销毁活动协程来取消它，应请求取消再 join。

## 跨层共用一个 deadline

`deadline{steady_clock::time_point}` 表示绝对时间，默认无限制；`deadline::after(duration)`
立即计算时间点，溢出饱和，零或负数已经过期。`earlier(a,b)` 选择更早期限。
`operation_options` 包含 `cancellation` 和 `deadline`。

`with_deadline(deadline, work)` 覆盖解析、连接、握手、收发和清理；父级、显式 token 和新 deadline
取最小值，内层不能重新计时或延长总预算。未包装的异步调用没有默认总超时。
协议原有的阶段超时仍可以提前失败，但不能延长总 deadline。
所有直接 HTTP/TCP/UDP 客户端的 `blocking_options` 也支持相同的 `cancellation/deadline`；
已有正数 `timeout`（默认 30 秒）保留为相对总预算简写，取更早的期限。

取消不能抢占同步 CPU 工作、系统调度或阻塞的外部调用；不合作的子任务会阻止 join 结束。
自定义 awaiter 通过 `io.on_cancel(token, callback)` 注册，持有返回 handle，完成时注销。
回调在所属事件循环运行，必须迅速结束并且恰好解除一次等待；handle 操作也须在所属线程，
或循环退出并 join 后执行。handle 可以比 context 活得更久。
需要 deadline 时使用活动操作边界提供的 token；context.stop 也会触发注册的取消。
回调异常遵守 reactor 的记录错误、停止、排空规则。

## 异步清理

`shield(work)` 屏蔽继承的局部取消，推荐
`shield(with_deadline(deadline::after(100ms), cleanup_factory))` 限制清理预算。
外层边界仍报告原取消；shield 不覆盖 `io.stop()`，也不把清理 detach。
边界最终排空会屏蔽局部取消，保证显式 join 被取消时也不会跳过子任务。
不要在子任务中 join 自己所属的组，否则会等待自己。主体退出时若仍有其他 join 挂起，
边界会拒绝这种误用，取消并等待子任务后再抛出。足够时优先使用同步 RAII 清理。

## 并发与服务端生命周期

所有 socket/协议对象属于单个 context，未完成操作期间不得移动或销毁。
取消源线程安全不等于 socket 方法线程安全。

| 组件 | 并发约定 |
|---|---|
| TCP / UDP | 一个读/接收和一个写/发送可重叠；同方向不得竞争 |
| TLS | 一个公开异步操作；HTTP/2 内部双工驱动独占 TLS 访问 |
| QUIC | 每连接一个协议驱动，不同流可并发；同一流同方向不得竞争 |
| HTTP/1 持久客户端 | 一次只能一个 send，不同 client 可并发 |
| HTTP/1/2/3 URL 客户端 | 每次独占新连接，不同调用可并发 |
| HTTP/2/3 批处理 | 一次调用独占 transport/session，在内部多路复用，结果按输入顺序返回 |
| handler | HTTP/1 每连接串行；HTTP/2/3 按配置上限并发 |

HTTP `serve` 通过任务组拥有已接受的连接。关闭 listener、父级取消或 context 关闭，都会取消并
等待这些工作再返回。HTTP/2、3 连接驱动退出前也会取消并等待活动 handler，替代旧的“连接结束后
handler 继续独立运行”行为。TCP 半关闭仍允许驱动在存活期间完成完整请求。
单个 HTTP 流 reset 可以丢弃结果；保证的是连接结束前 join，并非每次 reset 都单独取消该 handler。
用户 handler 必须配合取消，库无法强行中断阻塞回调。
