<!-- source-hash: 2ead4a2f96d3 -->
# `pace::task`

[English](task.md) · [运行时](README.zh.md)

## 概述

`task<T>` 是 PACE 所有异步调用使用的协程类型：惰性、只能移动、恰好有一个消费者的结果。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/runtime/task.hpp>`（也可用 `<pace/runtime.hpp>`） | `pace_runtime` | 始终构建 |

创建 task 时什么都不会执行。只有被 await（`co_await std::move(work)`）或作为根任务交给 context
（`io_context::spawn`、`pace::run`、`task_group::spawn`）时，协程体才开始运行。task 不自带执行器：
谁启动它，它就运行在谁的 context 上。

## 概要

```cpp
#include <pace/runtime.hpp>

#include <chrono>
#include <string>

using namespace std::chrono_literals;

pace::task<std::string> greet(std::string name) // name 由协程帧持有
{
    co_await pace::sleep_for(5ms);
    co_return "Hello, " + name;
}

pace::task<std::size_t> entry()
{
    auto pending = greet("PACE"); // 惰性：此时还没有运行任何代码
    const auto text = co_await std::move(pending);
    co_return text.size();
}

int main()
{
    return pace::run(entry) == 11 ? 0 : 1;
}
```

## 类型

### `task<T>` 与 `task<void>`

```cpp
template <class T = void>
class [[nodiscard]] task;
template <>
class [[nodiscard]] task<void>;
```

`T` 存放在 `std::optional<T>` 中，因此必须是 `std::optional` 接受的对象类型，不支持引用。两个特化的成员相同：

```cpp
using handle_type = std::coroutine_handle<promise_type>;
task() noexcept;
task(task&& other) noexcept;
task& operator=(task&& other) noexcept;
task(const task&) = delete;
task& operator=(const task&) = delete;
~task();
handle_type release() noexcept;
awaiter operator co_await() && noexcept;
```

| 成员 | 行为 |
|:--|:--|
| `task()` | 空 task，没有协程帧。await 它会抛出 `std::logic_error`。 |
| 移动构造 | 接管协程帧，源对象变为空。 |
| 移动赋值 | 先销毁目标原有的协程帧，再接管源对象的协程帧。 |
| `~task()` | 销毁仍然持有的协程帧及其参数和局部对象，不会运行尚未开始的协程体，也不等待任何操作。 |
| `release()` | 清空 task 并返回原始句柄（可能为空）。调用方此后必须自行恢复、完成并恰好销毁一次该协程帧。它既不调度 task，也不能安全地 detach。 |
| `operator co_await() &&` | 把协程帧移交给一个拥有它的 `awaiter`。只能 await 右值：具名 task 要写 `co_await std::move(work)`。 |

没有左值 await，也没有 `get()`、`result()`、`done()` 或到 `bool` 的转换。

### `task<T>::awaiter`

```cpp
struct awaiter
{
    handle_type handle;
    awaiter(handle_type h) noexcept;
    awaiter(awaiter&& other) noexcept;
    awaiter(const awaiter&) = delete;
    awaiter& operator=(const awaiter&) = delete;
    awaiter& operator=(awaiter&&) = delete;
    ~awaiter();
    bool await_ready() const noexcept;
    template <class P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> parent);
    T await_resume(); // task<void> 为 void
};
```

awaiter 持有协程帧，并在 `co_await` 表达式结束时销毁它。

- 对空的或已经结束的协程帧，`await_ready` 为真。
- `await_suspend` 把父任务的取消令牌交给子任务（除非操作边界已经给子任务绑定了自己的令牌）；若该令牌已被取消，
  就抛出 `std::system_error`，子任务的协程体永远不会开始；否则通过对称转移把控制权交给子任务。
- `await_resume` 对空 task 抛出 `std::logic_error`；重新抛出从子任务协程体逃出的异常；若子任务结束时其令牌已被取消，
  抛出 `std::system_error`；否则把结果移出。

最后一条意味着：子任务结束时观察到的取消优先于它返回的值。

### `promise_type`

```cpp
// task<T>::promise_type
struct promise_type : promise_base
{
    std::optional<T> value;
    task get_return_object() noexcept;
    template <class U>
    void return_value(U&& result);
};

// task<void>::promise_type
struct promise_type : promise_base
{
    std::size_t root_index = 0; // 仅在 io_context::spawn 持有该协程帧时使用
    task get_return_object() noexcept;
    void return_void() const noexcept;
};
```

编译器用它们构造协程。`return_value` 用 `co_return` 的操作数构造 `value`。

### `promise_base`

```cpp
struct promise_base
{
    cancellation_token cancellation;
    bool cancellation_bound = false;
    std::coroutine_handle<> continuation = std::noop_coroutine();
    std::exception_ptr exception;
    std::coroutine_handle<> completed_next{};
    void* completion_context = nullptr;
    void (*on_completion)(void*, std::coroutine_handle<>) noexcept = nullptr;
    std::suspend_always initial_suspend() const noexcept;
    void unhandled_exception() noexcept;
    struct final_awaiter;
    final_awaiter final_suspend() const noexcept;
};
```

公共的 promise 基类。`initial_suspend` 让每个 task 都是惰性的；`unhandled_exception` 保存逃出的异常供
`await_resume` 使用；`final_suspend` 恢复 `continuation`（正在 await 的父任务；根任务则没有）。
`cancellation` 是协程帧观察的令牌，`this_task::cancellation()` 读取的就是它。其余字段是 `io_context`
管理根任务用的记录，应用代码不要触碰。

### `task_access`

```cpp
struct task_access
{
    template <class T>
    static task<T> bind(task<T> work, cancellation_token token);
};
```

供操作边界和任务组使用的实现支持：`bind` 给尚未开始的 task 指定自己的令牌，此后它保留这个令牌，不再继承父任务的。
应用代码请使用 [`with_cancellation`](structured-concurrency.zh.md) 或 [`shield`](structured-concurrency.zh.md)。

## 函数

### `this_task::cancellation`

```cpp
namespace pace::this_task
{
cancellation_awaiter cancellation() noexcept;
}
```

`co_await pace::this_task::cancellation()` 返回当前 task 的 `cancellation_token` 副本，不挂起，也不抛出，
即使令牌已被取消。可以在清理时用它检查 `stop_requested()`，或把令牌交给自定义等待。
若协程的 promise 没有 PACE 令牌，结果是空令牌。awaiter 类型 `this_task::cancellation_awaiter`
有一个公开的 `cancellation_token value` 成员和常规的 `await_ready`/`await_suspend`/`await_resume`，
请把它当作不透明类型。

## 错误

| 情形 | 抛出 |
|:--|:--|
| await 空的或已被移走的 task | `co_await` 抛出 `std::logic_error` |
| 子任务协程体抛出异常 | `co_await` 重新抛出同一异常 |
| 继承的令牌在子任务开始前或结束时已被取消 | 带令牌错误码的 `std::system_error`（`std::errc::operation_canceled`，截止时间到期则为 `std::errc::timed_out`） |

从根任务（交给 `io_context::spawn` 的 task）逃出的异常不会丢失：context 记录它并停止，见
[`io_context`](io-context.zh.md#错误)。

## 线程与并发

task 在开始前不绑定任何线程；开始后运行在恢复它的 context 上。await 一个 task 不会切换线程，也不保证经过一次调度：
没有挂起就完成的子任务会直接继续执行父任务。task 中 await 的一切都必须属于它所在的 context。

一个 task 只有一个消费者。await 即消费；不能 await 同一个 task 两次，也不能在两个协程中 await 它。

## 生命周期与所有权

- 按值传入的参数（`std::string`、`tcp::stream` 等）被移入协程帧，存活到 task 结束或被销毁。
- 引用、指针、`std::span` 和 `std::string_view` 参数是借用：协程帧不会延长被引用对象的生命周期，
  该对象必须比 task 活得更久。
- lambda 协程的捕获存放在闭包对象中，而不是协程帧中。立即调用临时的捕获 lambda（`[x] () -> task<void> { … }()`）
  会让 task 指向已销毁的闭包。应把 lambda 本身交给会持有它的 API（`pace::run`、`with_options`、
  `task_group::spawn`），或者使用按值传参的具名协程函数。
- 销毁尚未开始的 task 是安全的。销毁挂起在已注册等待中的协程帧不是取消它的方式：请请求取消，让它自行结束。

## 限制与默认值

| 项目 | 值 |
|:--|:--|
| 默认 `T` | `void` |
| 每个 task 的消费者 | 1 |
| 支持的 `T` | `std::optional<T>` 接受的对象类型 |

## 参见

- [`io_context`](io-context.zh.md)：运行根任务、定时器与就绪等待
- [`pace::run`](run.zh.md)：在 `main` 中把一个 task 运行到结束
- [结构化并发](structured-concurrency.zh.md)：令牌、截止时间与任务组
- [教程：任务与定时器](../../tutorial/04-tasks-and-timers.zh.md)
- [所有权](../../explanation/ownership.zh.md)
