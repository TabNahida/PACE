<!-- source-hash: 91b015fb5b7a -->
# 运行时

[English](README.md) · [参考](../README.zh.md)

## 概述

运行时模块负责运行 PACE 的协程：惰性 task、带定时器和就绪等待的单线程 `io_context` 反应器、多线程 runtime、
同步入口 `pace::run`、取消、截止时间和结构化任务组。其他所有模块都构建在它之上，它没有第三方依赖。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/runtime.hpp>` | `pace_runtime` | 始终构建 |
| `<pace/pace.hpp>` | `pace` | 始终构建；包含本次构建启用的模块 |

使用 C++ 命名模块（`--modules=y`）时，`import pace.runtime;` 导出本模块面向应用的名字，`import pace;` 导出所有模块。

### 页面

| 页面 | 头文件 | 内容 |
|:--|:--|:--|
| [`task`](task.zh.md) | `runtime/task.hpp` | `task<T>`、其 awaiter 和 promise、`this_task::cancellation` |
| [`io_context`](io-context.zh.md) | `runtime/io_context.hpp`、`runtime/platform.hpp` | 反应器：`spawn`、`post`、`run`、`stop`、定时器、就绪等待、`on_cancel`、`native_socket_type` |
| [`runtime`](runtime.zh.md) | `runtime/runtime.hpp` | 每个工作线程一个 context |
| [`run`](run.zh.md) | `runtime/run.hpp`、`runtime/this_context.hpp`、`runtime/blocking.hpp` | `pace::run`、`this_context`、`sleep_for`、`yield`、`blocking_options`、`blocking_call` |
| [结构化并发](structured-concurrency.zh.md) | `runtime/task_group.hpp`、`runtime/operation.hpp`、`runtime/cancellation.hpp` | `task_group`、`join_handle`、`with_task_group`、`with_options`、`with_deadline`、`with_cancellation`、`shield`、`deadline`、`cancellation_source`、`cancellation_token` |
| [I/O 后端](io-backends.zh.md) | `runtime/io_backend.hpp` | `io_backend`、`backend_name`、`PACE_IO_BACKEND` |
| [句柄与信号](handles-and-signals.zh.md) | `runtime/io_context.hpp`、`runtime/signal.hpp` | 管道、终端与 TUN 设备上的就绪等待；Windows `wait_handle`；`process_signal`、`signal_set`、`wait_for_signal`、`deliver_signal`、`signal_name` |
| 本页 | `runtime/ascii.hpp`、`pace.hpp` | `ascii::to_lower`、`ascii::iequals`；包含所有模块的头文件 |

所有头文件都位于 `include/pace/` 下。`<pace/runtime.hpp>` 包含除 `pace.hpp` 之外的全部头文件。

`<pace/pace.hpp>` 包含 `<pace/runtime.hpp>`、`<pace/net.hpp>` 和 `<pace/http.hpp>`，以及本次构建启用的可选模块的头文件
（TLS、DNS、QUIC、HTTP/1 及网关、HTTP/2、HTTP/3、WebSocket）。它自身不声明任何东西。请优先包含所用模块的头文件。

## 概要

```cpp
#include <pace/runtime.hpp>

#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

pace::task<int> slow_answer()
{
    co_await pace::sleep_for(10ms);
    co_return 42;
}

pace::task<int> entry()
{
    // 内部所有工作共用一个预算；预算耗尽时调用方收到 std::system_error(timed_out)。
    co_return co_await pace::with_deadline(pace::deadline::after(1s), slow_answer);
}

int main()
{
    std::cout << pace::run(entry) << '\n'; // 在本线程上使用一个新的 io_context
}
```

## 函数

### `ascii::to_lower`、`ascii::iequals`

```cpp
namespace pace::ascii
{
[[nodiscard]] constexpr char to_lower(char c) noexcept;
[[nodiscard]] constexpr bool iequals(std::string_view a, std::string_view b) noexcept;
[[nodiscard]] inline std::string to_lower(std::string text);
}
```

与区域设置无关的 ASCII 大小写折叠，用于 HTTP 字段名、DNS 名称等协议标记。只改变 `A`–`Z`，因此 UTF-8
和其他字节原样通过。`to_lower(char)` 折叠一个字符，`to_lower(std::string)` 返回折叠后的副本，
`iequals` 忽略 ASCII 大小写比较两个字符串。

## 错误

以下规则适用于所有基于运行时的 API。

**被 await 的 task 会重新抛出异常。** 从 task 逃出的异常在 await 它的地方重新抛出。根任务（交给 `io_context::spawn`
的 task）或投递回调的异常若无人捕获，会停止其 context；`run()` 在 context 排空后重新抛出第一个这样的异常，
[`runtime`](runtime.zh.md) 则停止所有 context，在 join 之后重新抛出它。除了各页面列出的错误外，
几乎任何调用都可能传播内存分配失败。

**取消是协作式的。** `io_context::stop()` 是永久的请求，不是 join，也不是强制中断：挂起的就绪等待和定时器以错误码等于
`std::errc::operation_canceled` 的 `std::system_error` 恢复，task 应当随之结束。在预期会取消的地方捕获该错误码。
context 停止期间从根任务逃出的 `operation_canceled` 是预期结果，`run()` 不会报告它；其他异常仍会报告。
[令牌和截止时间](structured-concurrency.zh.md)以同样的方式取消部分工作，错误码为 `operation_canceled` 或
`std::errc::timed_out`，但不会停止 context。

## 线程与并发

**每个 context 一个线程。** 每个 `io_context` 由一个线程运行，其 socket、定时器和等待只在该线程上使用。`spawn`、`post`
和 `stop` 是跨线程入口。在 `run()` 之前创建对象、在它返回之后销毁对象都没有问题；在它运行期间从无关线程访问这些对象则不行，
只读访问器也不例外。各协议页面说明一个对象上可以同时有多少个未完成操作。

**阻塞工作会饿死反应器。** 协程一直运行到挂起或返回。阻塞调用或长时间的 CPU 循环会让该 context 上的所有其他 task、
定时器和 I/O 停滞，并使关闭无法完成。空闲的 context 会一直运行到被停止，且 context 不能重新启动。

## 生命周期与所有权

- **task 是惰性的，只能消费一次。** 创建 task 不运行任何东西；await 或 spawn 它就消费了它。
- **按值参数由协程帧持有，引用则不然。** 按值传入的参数成为协程帧的一部分。引用、指针、span 和 string view
  不会延长其对象的生命周期。请优先使用按值传参的具名协程函数，并把捕获 lambda 交给会持有它们的 API
  （`pace::run`、`with_options`、`task_group::spawn`），而不是自己调用。
- **context 比绑定到它的对象活得更久。** socket、listener 和挂起的操作不能比其 context 活得更久，
  socket 在其操作完成之前必须保持地址不变。

## 参见

- [教程：任务与定时器](../../tutorial/04-tasks-and-timers.zh.md)、
  [结构化并发](../../tutorial/07-structured-concurrency.zh.md)、
  [工作线程](../../tutorial/08-workers-and-threads.zh.md)、
  [优雅关闭](../../tutorial/09-graceful-shutdown.zh.md)
- [执行模型](../../explanation/execution-model.zh.md) · [所有权](../../explanation/ownership.zh.md)
- [网络](../net/README.zh.md)：运行时之上的 TCP 和 UDP
- [构建选项](../build-options.zh.md)
