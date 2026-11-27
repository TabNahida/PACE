<!-- source-hash: cb95a6beac79 -->
# `pace::run` 与当前 context

[English](run.md) · [运行时](README.zh.md)

## 概述

`pace::run` 在调用线程上用一个新的 `io_context` 把一个 task 运行到结束并返回其结果；与它并列的辅助函数用于找到
task 正在运行的 context，以及把异步调用变成阻塞调用。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/runtime/run.hpp>`（也可用 `<pace/runtime.hpp>`） | `pace_runtime` | 始终构建 |
| `<pace/runtime/this_context.hpp>`（也可用 `<pace/runtime.hpp>`） | `pace_runtime` | 始终构建 |
| `<pace/runtime/blocking.hpp>`（也可用 `<pace/runtime.hpp>`） | `pace_runtime` | 始终构建 |

`run` 不创建线程，也没有全局事件循环：每次调用都有自己的 context，调用返回时它随之结束。

## 概要

```cpp
#include <pace/runtime.hpp>

#include <chrono>
#include <iostream>
#include <string>

using namespace std::chrono_literals;

int main()
{
    std::string name = "PACE";
    // 传入 lambda 本身：run 持有它及其捕获，直到 context 排空。
    auto text = pace::run(
        [name]() -> pace::task<std::string>
        {
            co_await pace::sleep_for(1ms);
            co_return "Hello, " + name;
        }
    );
    std::cout << text << '\n';
}
```

## 类型

### `blocking_options`

```cpp
struct blocking_options
{
    std::chrono::milliseconds timeout{30000}; ///< Total budget from the start of the call; must be positive.
    cancellation_token cancellation{};        ///< Cancels the call; may be cancelled from another thread.
    pace::deadline deadline{};                ///< Absolute deadline; none by default.
};
```

所有阻塞（“直接”）调用的选项，例如 `http::get`、`tcp::exchange` 和 `udp::exchange`。

| 成员 | 含义 |
|:--|:--|
| `timeout` | 从调用开始计算的总时间预算。必须为正数。 |
| `cancellation` | 用于取消调用的令牌，可以在其他线程上取消。 |
| `deadline` | 绝对截止时间。调用的预算在 `deadline` 与“当前时间 + `timeout`”中较早者结束。 |

预算覆盖整个调用，例如域名解析、建立连接、握手、交换数据和清理。异步形式（`async_get`、`tcp::async_exchange` 等）
不接受 `blocking_options`，也不添加截止时间：它们继承调用方的令牌和截止时间，局部截止时间用
[`with_options`](structured-concurrency.zh.md) 设置。

### 入口支持

```cpp
struct entry_access
{
    template <class Factory>
    static auto invoke(Factory& factory, io_context& io);
    template <class Task>
    using result = /* T for task<T> */;
    template <class T>
    struct state;                  // value (absent for void), failure, completed
};

template <class T, class Factory>
task<void> run_entry(io_context& io, Factory& factory, entry_access::state<T>& outcome);
```

`run`、操作边界和任务组共用的实现支持。`factory(io)` 合法时 `entry_access::invoke` 调用它，否则调用 `factory()`；
`entry_access::result` 给出工厂所返回 task 的结果类型。`run_entry` 是 `pace::run` spawn 的根任务：它 await 工厂的
task，把值或异常记录到 `outcome`，结束时停止 `io`；它是命名空间作用域的函数，因为 GCC 16 无法从具名模块接口中读回
类作用域的协程。不属于 API；应用代码请使用 `pace::run`。

## 函数

### `run`

```cpp
template <class Factory>
requires(std::is_invocable_v<std::decay_t<Factory>&, io_context&> || std::is_invocable_v<std::decay_t<Factory>&>)
auto run(Factory&& factory);

template <class T>
auto run(task<T> entry);
```

创建一个 `io_context`，在调用线程上用它运行入口，并返回入口的结果：`task<T>` 返回 `T`（移出，因此支持只能移动的结果），
`task<void>` 不返回任何东西。

- **工厂形式。** `factory` 是接受 `io_context&` 或无参数、返回 `task<T>` 的可调用对象；两种调用都合法时使用
  `io_context&` 形式。它被复制（左值）或移动（右值）到 `run` 持有的存储中，只调用一次，并一直保留到 context 排空。
  因此在这里传入临时的捕获协程 lambda 是安全的，前提是传入 lambda 本身，而不是自己调用它。
- **task 形式。** `entry` 是已经创建好的 task。`run` 持有该 task，但 task 借用的东西仍是借用：
  调用临时捕获 lambda 得到的 task 已经指向被销毁的闭包。

入口结束时（无论成功与否），context 被停止：后台任务（用 `this_context().spawn(…)` 创建的根任务）挂起的等待和定时器被取消，
就绪的工作在 `run` 返回前排空。这不是对后台工作的 join，也不是连接的优雅排空。

### `this_context`

```cpp
inline io_context& this_context();
```

调用线程上正在运行的 context（`io_context::current()`）。那里没有正在运行的 PACE context 时抛出 `std::logic_error`；
它从不创建 context。其他模块中不接受 `io_context&` 的便捷重载（`tcp::connect(endpoint)`、`dns::resolve(name)` 等）
使用它：异步调用在其 task 开始时调用它，`tcp::listen` 这类同步调用则在被调用时调用它。

### `sleep_for` 与 `yield`

```cpp
template <class Rep, class Period>
auto sleep_for(std::chrono::duration<Rep, Period> duration);

inline auto yield();
```

即 `this_context().sleep_for(duration)` 和 `this_context().yield()`：在调用时解析 context，没有正在运行的 context
时抛出 `std::logic_error`，并返回与 [`io_context`](io-context.zh.md) 成员相同的 awaiter，不分配内存。
请立即在同一个 context 上 await 其结果。

### `blocking_call`

```cpp
template <class Factory>
auto blocking_call(Factory&& factory, blocking_options options);
```

在一个操作边界内用 `pace::run` 运行 `factory` 的 task 并返回其结果；边界的取消令牌是 `options.cancellation`，
截止时间是 `options.deadline` 与“当前时间 + `options.timeout`”中较早者。net 和 HTTP 模块的阻塞调用就是这样构建的，
它也适用于 `pace::run` 接受的任何工厂：

```cpp
auto value = pace::blocking_call(
    []() -> pace::task<int>
    {
        co_await pace::sleep_for(10ms);
        co_return 7;
    },
    {.timeout = 1s}
);
```

## 错误

| 错误 | 时机 |
|:--|:--|
| `std::logic_error` | 在已运行 PACE context 的线程上调用 `run` 或 `blocking_call`（在调用工厂之前检查：此时应改为 await task）；在没有运行中 context 的地方调用 `this_context`、`sleep_for` 或 `yield` |
| 入口的异常 | 入口 task 抛出了异常，包括表示其自身被取消的 `std::system_error` |
| `std::system_error(operation_canceled)` | 入口没有结束，例如 context 在它运行时被停止 |
| 后台错误 | context 上 spawn 的某个根任务失败；见 [`io_context`](io-context.zh.md#错误) 的反应器错误策略 |
| `std::invalid_argument` | `blocking_call` 的 `timeout <= 0` |
| `std::system_error(timed_out)` | `blocking_call` 的预算耗尽，包括工作在截止时间之后才完成的情况 |
| `std::system_error(operation_canceled)` | `blocking_call` 的 `options.cancellation` 被取消 |

`run` 只报告一个失败。入口和后台任务都失败时，重新抛出哪一个没有规定。`io_context::run` 把关闭期间的
`operation_canceled` 视为预期结果，而 `run` 会把入口的取消报告给调用方。

阻塞调用的预算是协作式的：CPU 工作、加载信任存储这类同步准备工作和阻塞的外部代码不会被中断，可能使调用超出预算。
不会自动重试，已经发送的字节也无法收回。没有后台工作会比调用活得更久。

## 线程与并发

`run` 和 `blocking_call` 阻塞调用线程并在其上运行一切，不创建工作线程。它们拒绝在已运行 PACE context 的线程上运行，
但检测不到其他事件循环：在 GUI 或其他库的事件循环线程上调用时，会阻塞那个循环。不同线程上的调用彼此独立。

`this_context`、`sleep_for` 和 `yield` 读取调用线程的当前 context；它们返回的 awaiter 属于该 context，必须在那里 await。

## 生命周期与所有权

- `run` 持有工厂直到关闭完成，出错时也是如此。
- `run` 返回时 context 随之结束。请返回拥有数据的值（`std::string`、数据结构体），不要返回引用该 context
  或工厂捕获的 socket、流或视图。
- 后台根任务必须拥有自己的数据：入口的局部变量可能在这些根任务观察到取消之前就已销毁。
- `blocking_options::cancellation` 被复制；调用运行期间，其来源可以在任何线程上被取消。

## 限制与默认值

| 项目 | 值 |
|:--|:--|
| `blocking_options::timeout` | 30 秒（30000 毫秒）；必须为正数 |
| `blocking_options::deadline` | 无 |
| `blocking_options::cancellation` | 无（空令牌） |
| `run` 创建的线程 | 0 |
| 嵌套 `run` | 被拒绝 |

## 参见

- [`io_context`](io-context.zh.md)：`run` 创建并停止的对象
- [`task`](task.zh.md)：task 参数的所有权
- [结构化并发](structured-concurrency.zh.md)：`with_options`、截止时间与取消
- [HTTP 客户端](../http/clients.zh.md)、[TCP](../net/tcp.zh.md)、[UDP](../net/udp.zh.md)：接受
  `blocking_options` 的阻塞调用
- [教程：任务与定时器](../../tutorial/04-tasks-and-timers.zh.md)
