<!-- source-hash: e346edb18da1 -->
# 结构化并发、取消与截止时间

[English](structured-concurrency.md) · [运行时](README.zh.md)

## 概述

任务组在一个会 join 子任务的作用域内并发运行子任务；取消令牌和截止时间以协作方式停止工作，而不停止 context；
操作边界为任意 task 附加令牌和截止时间。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/runtime/task_group.hpp>`（也可用 `<pace/runtime.hpp>`） | `pace_runtime` | 始终构建 |
| `<pace/runtime/operation.hpp>`（也可用 `<pace/runtime.hpp>`） | `pace_runtime` | 始终构建 |
| `<pace/runtime/cancellation.hpp>`（也可用 `<pace/runtime.hpp>`） | `pace_runtime` | 始终构建 |

每个 task 都带有一个 `cancellation_token`。await 一个 task 会把调用方的令牌向下传递；操作边界（`with_options`、
`with_deadline`、`with_cancellation`、`with_task_group`）创建一个与之关联的新令牌，截止时间取继承的与新设的二者中较早者。
取消令牌会以 `std::system_error`（`operation_canceled`，截止时间到期则为 `timed_out`）唤醒在其下注册的等待；
context 继续运行。

## 概要

```cpp
#include <pace/runtime.hpp>

#include <chrono>
#include <iostream>
#include <string>

using namespace std::chrono_literals;

pace::task<std::string> fetch_both()
{
    co_return co_await pace::with_task_group(
        [](pace::task_group& group) -> pace::task<std::string>
        {
            auto first = group.spawn(
                []() -> pace::task<std::string>
                {
                    co_await pace::sleep_for(10ms);
                    co_return "Hello";
                }
            );
            auto second = group.spawn(
                []() -> pace::task<std::string>
                {
                    co_await pace::sleep_for(5ms);
                    co_return "structured PACE";
                }
            );
            auto a = co_await first; // 若子任务抛出异常，在这里重新抛出
            auto b = co_await second;
            co_return a + ", " + b + "!";
        },
        {.deadline = pace::deadline::after(2s)} // 整个任务组共用一个预算
    );
}

int main()
{
    std::cout << pace::run(fetch_both) << '\n';
}
```

## 类型

### `deadline`

```cpp
class deadline
{
public:
    using clock = std::chrono::steady_clock;
    deadline() noexcept = default;
    explicit deadline(clock::time_point when) noexcept;
    template <class Rep, class Period>
    static deadline after(std::chrono::duration<Rep, Period> delay) noexcept;
    clock::time_point time() const noexcept;
    bool expired() const noexcept;
    friend deadline earlier(deadline a, deadline b) noexcept;
};
```

`steady_clock` 上的一个绝对时间点。

| 成员 | 行为 |
|:--|:--|
| `deadline()` | 没有截止时间：`time()` 为 `clock::time_point::max()`，永不到期。 |
| `deadline(when)` | 在 `when` 到期。 |
| `after(delay)` | 当前时间 + `delay`，在调用时计算。零和负的 `delay` 得到已经到期的截止时间；超出时钟范围的 `delay` 得到“没有截止时间”。 |
| `time()` | 时间点（没有截止时间时为 `max()`）。 |
| `expired()` | 截止时间存在且已经过去。 |
| `earlier(a, b)` | 两个截止时间中较早者；通过实参依赖查找找到。 |

### `cancellation_token`

```cpp
class cancellation_token
{
public:
    cancellation_token() noexcept = default;
    bool stop_possible() const noexcept;
    std::error_code error() const noexcept;
    bool stop_requested() const noexcept;
    void throw_if_cancelled() const;
    pace::deadline expires() const noexcept;
};
```

某个 `cancellation_source` 的廉价、可复制的观察者。默认构造的令牌为空，永远不会被取消。

| 成员 | 行为 |
|:--|:--|
| `stop_possible()` | 令牌是否属于某个来源。 |
| `error()` | 来源的取消原因（`operation_canceled` 或 `timed_out`）；否则若来源的截止时间已过则为 `timed_out`；否则为空错误码。 |
| `stop_requested()` | `error()` 非空。 |
| `throw_if_cancelled()` | 若 `stop_requested()`，抛出 `std::system_error(error())`。 |
| `expires()` | 来源的截止时间（空令牌没有截止时间）。 |

每次检查令牌都能看到已经过去的截止时间，但在那一刻不会有任何东西唤醒挂起的等待：负责唤醒的定时器由操作边界设置。

### `cancellation_source`

```cpp
class cancellation_source
{
public:
    explicit cancellation_source(pace::deadline expires = {});
    cancellation_token token() const noexcept;
    bool request_cancellation() const noexcept;
    bool request_timeout() const noexcept;
    void link(cancellation_token parent);
};
```

一个取消状态的所有者。副本共享同一状态。

| 成员 | 行为 |
|:--|:--|
| `cancellation_source(expires)` | 截止时间为 `expires` 的新状态。 |
| `token()` | 观察本来源的令牌。 |
| `request_cancellation()` | 以 `operation_canceled` 取消。若本次调用是第一个请求则返回 `true`；之后的请求不改变任何东西（第一个原因生效）。线程安全。 |
| `request_timeout()` | 同上，但原因为 `timed_out`；供截止时间定时器使用。 |
| `link(parent)` | `parent` 被取消时取消本来源：父级原因为 `timed_out` 时用 `timed_out`，否则用 `operation_canceled`；若 `parent` 已被取消则立即取消。只单向传播：取消本来源不影响 `parent`。不复制父级的截止时间。空的 `parent` 被忽略；把来源关联到它自己的令牌会抛出 `std::invalid_argument`。 |

请求可以来自任何线程。`link` 没有同步：请在来源被共享或工作开始之前完成关联。

### `cancellation_registration`

```cpp
class cancellation_registration
{
public:
    cancellation_registration(const cancellation_token&, std::function<void()>);
    ~cancellation_registration();
    cancellation_registration(const cancellation_registration&) = delete;
    cancellation_registration& operator=(const cancellation_registration&) = delete;
};
```

`link` 和 `io_context::on_cancel` 底层使用的订阅。回调只运行一次，运行在请求取消的线程上；若令牌已被取消，
则在构造函数内运行；空令牌不会注册任何东西。回调不能抛出异常，只应记录状态并唤醒某个对象。
析构函数注销回调，若回调正在另一线程上运行，则等它返回；回调可以销毁它自己的注册。
自定义 awaiter 应使用 [`io_context::on_cancel`](io-context.zh.md)，它会把回调放到 context 上运行。

### `operation_options`

```cpp
struct operation_options
{
    cancellation_token cancellation{};
    pace::deadline deadline{};
};
```

操作边界在继承的令牌和截止时间之外额外添加的令牌与截止时间。两者默认均为无。

### `task_group`

```cpp
class task_group
{
public:
    task_group(const task_group&) = delete;
    task_group& operator=(const task_group&) = delete;
    ~task_group() = default;

    template <class Factory>
    auto spawn(Factory factory);           // join_handle<T>
    template <class T>
    auto spawn(task<T> work);              // join_handle<T>
    template <class T>
    T& own(T value);
    void cancel();
    cancellation_token token() const noexcept;
    task<void> join();
    template <class Factory>
    static auto run(Factory factory) -> task<entry_access::result<std::invoke_result_t<Factory&, task_group&>>>;
};
```

一个 context 上的子任务作用域。只有 `task_group::run` 和 `with_task_group` 能创建它，并以引用形式交给主体；
它不能复制或移动，因此不会比 join 其子任务的作用域活得更久。

| 成员 | 行为 |
|:--|:--|
| `spawn(factory)` | 接管 `factory`（接受 `io_context&` 或无参数、返回 `task<T>` 的可调用对象），在之后的某一轮于本 context 上启动子任务，并返回 `join_handle<T>`。子任务的令牌就是任务组的令牌。 |
| `spawn(work)` | 对已创建的 task 做同样的事。任务组持有该 task；task 借用的东西仍是借用。 |
| `own(value)` | 把 `value` 移入任务组的存储，保留到所有子任务结束，并返回其引用。用它存放子任务借用的数据。 |
| `cancel()` | 取消任务组的令牌：主体和每个子任务在下一次等待时看到 `operation_canceled`。作用域仍会 join 它们，然后正常结束：由此产生的 `operation_canceled` 是所请求的结果，不算失败。某个子任务拿到答案后，用它停止其余子任务。 |
| `token()` | 任务组的令牌。 |
| `join()` | 一经调用就封闭任务组（不再允许 `spawn` 或 `own`），然后等待所有子任务结束。等待会观察任务组的令牌，因此任务组被取消时 `join()` 抛出异常而不是继续等待；无论哪种情况任务组都已封闭。它不会重新抛出子任务的异常。 |
| `run(factory)` | 作用域本身，一个惰性 task；见 `with_task_group`。 |

### `join_handle<T>`

```cpp
template <class T>
class [[nodiscard]] join_handle
{
public:
    join_handle(join_handle&&) noexcept = default;
    join_handle& operator=(join_handle&&) noexcept = default;
    join_handle(const join_handle&) = delete;
    join_handle& operator=(const join_handle&) = delete;
    ~join_handle() = default;
    struct awaiter;
    awaiter operator co_await() &;
    awaiter operator co_await() &&;
};
```

一个子任务的结果。`co_await handle` 等待子任务，返回其值或重新抛出其异常。句柄只能 await 一次，且必须在任务组的 context 上；
await 会移出结果并清空句柄。丢弃句柄既不会 detach 也不会取消子任务：任务组仍会 join 它，它的失败仍会让任务组失败。
嵌套的 `awaiter`（基于子任务共享状态的 `await_ready`、`await_suspend`、`await_resume`）是不透明的。

### 实现支持

```cpp
template <class Factory>
using operation_result = /* T of the task<T> that Factory returns */;

struct group_access
{
    struct state;                                  // the group's shared state
    template <class T> struct child;               // one child's shared state
    template <class Factory> struct owned_child;   // child state plus its owned factory
    struct wait;                                   // waits until no child is running
};

template <class Factory>
task<void> run_group_child(std::shared_ptr<group_access::state> group,
                           std::shared_ptr<group_access::owned_child<Factory>> owned);
task<void> drain_group(std::shared_ptr<group_access::state> group, bool boundary = false);
template <class Factory>
task<operation_result<Factory>> scoped_operation(Factory factory, operation_options options);  // with_options
```

因为上面的模板要用到它们，所以声明在公开头文件中：任务组和每个子任务的共享状态（集中在 `group_access` 中）、
运行子任务的根任务、等待任务组排空的 task，以及 `with_options` 背后的协程。这些协程是命名空间作用域的函数，而不是
`group_access` 的成员，因为 GCC 16 无法从具名模块接口中读回类作用域的协程。它们都不属于 API。`operation_result<Factory>` 是边界或 `spawn` 针对
`Factory` 返回的结果类型。

## 函数

### `with_options`、`with_deadline`、`with_cancellation`

```cpp
template <class Factory>
auto with_options(Factory factory, operation_options options);
template <class T>
auto with_options(task<T> work, operation_options options);
template <class Work>
auto with_deadline(pace::deadline when, Work work);
template <class Work>
auto with_cancellation(cancellation_token token, Work work);
```

操作边界：一个惰性 `task<T>`，在新的取消来源下运行 `work`（接受 `io_context&` 或无参数的工厂，或一个 `task<T>`）
并返回其结果。`with_deadline` 和 `with_cancellation` 就是只设置一个字段的 `with_options`。

边界开始时：

1. 创建一个来源，其截止时间取继承令牌、`options.cancellation` 和 `options.deadline` 三者中最早者，
   因此嵌套边界只能缩短预算，不能延长；
2. 把它关联到继承的令牌和 `options.cancellation`，二者任一都能取消工作；
3. 若结果已被取消则立即抛出（工作永远不会被调用）；
4. 在当前 context 上设置回调定时器，在截止时间请求 `timed_out`；
5. 用新令牌运行工作，结束后若来源已被取消或截止时间已过则抛出异常，即使工作返回了值。

取消只向内传播：内层截止时间到期不会取消调用方，调用方只会收到 `timed_out` 异常。边界持有 `work` 直到它结束；
请传入捕获 lambda 本身，而不是调用它得到的 task。其他模块的 `async_` 调用本身不添加总截止时间，需要用边界包裹来限定。
协议的分阶段超时仍可能更早失败，但没有什么能延长边界的预算。

### `shield`

```cpp
template <class Work>
auto shield(Work work);
```

在没有继承令牌、也没有继承截止时间的情况下运行 `work`，用于即使调用方正在被取消也必须执行的清理。
`io_context::stop()` 仍然生效。请给它设定自己的截止时间：

```cpp
co_await pace::shield(pace::with_deadline(pace::deadline::after(100ms), send_goodbye));
```

`shield` 不会 detach：调用方等待清理完成，之后在下一个取消点报告自身的取消。同步的 RAII 清理足够时优先使用它。

### `with_task_group`

```cpp
template <class Factory>
auto with_task_group(Factory factory, operation_options options = {});
```

一个惰性 `task<T>`：在当前 context 上创建 `task_group`，把 `factory(group)`（返回 `task<T>`）作为主体运行，
join 所有子任务，并返回主体的结果。`options` 非空时，作用域运行在 `with_options` 之内。`task_group::run(factory)`
是没有选项的同一作用域。

作用域如何结束：

1. 任务组的令牌关联到继承的令牌，并带有其截止时间。
2. 第一个从主体或子任务逃出的异常被记录并取消任务组，于是兄弟任务和主体看到 `operation_canceled`。
   后续异常被丢弃；错误不会聚合。
3. 主体返回或抛出后，作用域等待每个子任务，这次等待忽略取消。
4. 然后销毁 `own` 创建的存储。
5. 重新抛出记录的异常；否则若外层操作已被取消（继承的令牌或截止时间），以该原因抛出 `std::system_error`；
   否则返回主体的值。

任务组自己的 `cancel()` 不算错误。主体或子任务因它而抛出的 `operation_canceled` 不会被记录，作用域正常返回：
`void` 主体的作用域直接返回，有返回值的主体的作用域返回主体产生的值。如果主体没有产生值就结束了（它自己也被取消），
作用域抛出 `operation_canceled`。主体或子任务抛出的其他异常仍会被记录并重新抛出。于是“谁先完成谁赢”就是一个由
胜出的子任务取消的任务组：

```cpp
auto winner = co_await pace::task_group::run(
    [&](pace::task_group& group) -> pace::task<std::string>
    {
        auto& answer = group.own(std::string{});
        for (auto& mirror : mirrors)
        {
            (void)group.spawn(
                [&group, &answer, &mirror]() -> pace::task<void>
                {
                    answer = (co_await pace::http::async_get(mirror)).body;
                    group.cancel();
                }
            );
        }
        try
        {
            co_await group.join();
        }
        catch (const std::system_error&)
        {
            // join 报告结束这场竞速的取消
        }
        co_return answer;
    }
);
```

由于子任务的异常会传播到作用域之外，嵌套任务组的失败会让运行它的子任务失败；要隔离错误，请在子任务内部捕获。

## 错误

| 错误 | 时机 |
|:--|:--|
| `std::system_error(operation_canceled)` | 令牌被来源、关联的父级、`task_group::cancel()` 或兄弟任务的失败取消时的等待；在已取消的任务组上 `spawn` |
| `std::system_error(timed_out)` | 边界的截止时间已过，无论发生在工作期间还是返回结果之前 |
| 主体或子任务的第一个异常 | 所有子任务结束后由 `with_task_group` 重新抛出 |
| `std::logic_error` | 在任务组的 context 之外调用 `spawn`、`own`、`cancel`、`join` 或 await `join_handle`；`join` 开始后调用 `spawn` 或 `own`；并发的第二个 `join`；主体返回时仍有 `join`（例如来自子任务）挂起；await 空的或已被 await 过的 `join_handle` |
| `std::invalid_argument` | 用来源自己的令牌调用 `cancellation_source::link` |

取消不是回滚。取消读操作不会取消同一描述符上的写操作，socket 保持打开，字节可能已经发送或接收。
各协议的页面说明取消会给其对象留下什么状态。

## 线程与并发

- `cancellation_source::request_cancellation` 和 `request_timeout` 可以从任何线程调用。PACE 为请求注册的回调只标记工作
  并唤醒所属反应器；等待在各自的 context 上被注销和恢复。
- 这里的其他一切（边界、任务组、句柄）都运行在一个 context 上。任务组是单个反应器上的协作并发，不是线程池：
  阻塞或死循环的子任务会占住所有其他子任务。
- 取消是协作式的。它不能中断 CPU 工作、操作系统调度或阻塞的外部代码；永远到不了取消点的子任务会让作用域无法结束。
- 等待完成后，若 task 恢复时其令牌已被取消，取消优先于结果。
- 不要在任务组自己的子任务中 join 它：那会等待自己。主体退出时作用域会在取消并排空子任务后拒绝这种用法。
- `io_context::spawn` 仍是非结构化的根任务：它不继承令牌，也没有人 join 它。属于某个作用域的工作请用 `task_group::spawn`。

## 生命周期与所有权

- 边界和任务组持有各自的工厂直到工作结束；子任务的工厂及其捕获在 join 完成前销毁。
- 可以借用整个作用域之外的调用方数据：作用域返回前调用方一直挂起。
- 主体的局部变量在主体返回或抛出时销毁，可能早于子任务结束。子任务必须按值捕获，或借用 `group.own` 创建的存储。
- 从作用域返回拥有数据的值，不要返回指向其存储的视图。
- 不要销毁正在运行的协程来取消它：请请求取消并 join 它。

## 限制与默认值

| 项目 | 值 |
|:--|:--|
| `operation_options` | 无令牌，无截止时间 |
| `cancellation_source` 截止时间 | 无 |
| `with_task_group` 选项 | 无 |
| 每个任务组的子任务数 | 没有固定上限 |
| 每个 `join_handle` 的 await 次数 | 1 |
| 每个任务组保留的错误 | 第一个 |

## 参见

- [`task`](task.zh.md)：`co_await` 时的令牌继承
- [`io_context`](io-context.zh.md)：供自定义 awaiter 使用的 `on_cancel` 和回调定时器
- [`pace::run`](run.zh.md)：`blocking_options` 与 `blocking_call`
- [教程：结构化并发](../../tutorial/07-structured-concurrency.zh.md) ·
  [优雅关闭](../../tutorial/09-graceful-shutdown.zh.md)
- [执行模型](../../explanation/execution-model.zh.md)
