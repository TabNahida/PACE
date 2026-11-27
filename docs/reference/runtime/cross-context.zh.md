<!-- source-hash: 120075cd102d -->
# 跨 context 通信

[English](cross-context.md) · [运行时](README.zh.md)

## 概述

用于跨越多个 context 的工作：任意线程都能发送、由一个 context 接收的有界 channel，把不可变值发布到每个 context 的
snapshot，在另一个 context 上等待工作结果的 `on()`，以及 `runtime` 上固定的键到 context 映射。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/runtime/channel.hpp>`（也可用 `<pace/runtime.hpp>`） | `pace_runtime` | 始终构建 |
| `<pace/runtime/snapshot.hpp>`（也可用 `<pace/runtime.hpp>`） | `pace_runtime` | 始终构建 |
| `<pace/runtime/on.hpp>`（也可用 `<pace/runtime.hpp>`） | `pace_runtime` | 始终构建 |

每个 context 仍然在自己的线程上运行自己的协程。这些工具在 context 之间传递值和恢复，从不搬动协程：发送方、
读者和调用方总是在它原来所在的 context 上恢复。

| 需求 | 使用 |
|:--|:--|
| 许多生产者为一个 context 拥有的状态（对端表、TUN 设备、配额）提供数据 | 由该 context 持有的 `channel<T>` |
| 每个 context 读取同一份配置，偶尔替换 | `snapshot<T>` |
| 在另一个 context 上计算结果，像本地 `co_await` 一样使用 | `co_await on(context, work)` |
| 选出拥有某个键的 context | `runtime::shard_for(key)`（[`runtime`](runtime.zh.md#runtime)） |

## 概要

```cpp
#include <pace/runtime.hpp>

#include <atomic>
#include <iostream>
#include <memory>
#include <string>

struct routes
{
    std::string upstream;
};

int main()
{
    pace::runtime workers(4);
    auto& owner = workers.shard_for(std::string("peer-table"));
    pace::channel<int> updates(owner, 1024); // 在 `owner` 上接收，可从任何地方发送
    pace::snapshot<routes> config(workers, std::make_shared<const routes>(routes{"10.0.0.1"}));
    std::atomic<int> producers{3};

    auto consumer = [&]() -> pace::task<void>
    {
        int total = 0;
        while (auto value = co_await updates.receive()) // 关闭且取空之后为 std::nullopt
        {
            total += *value;
        }
        // 在另一个 context 上工作，带着结果回到这里恢复。
        auto upstream = co_await pace::on(
            workers.context(0),
            [&]() -> pace::task<std::string> { co_return config.get()->upstream; }
        );
        std::cout << total << ' ' << upstream << '\n';
        workers.stop();
    };
    auto producer = [&](int value) -> pace::task<void>
    {
        co_await updates.send(value); // channel 满时等待
        config.publish(std::make_shared<const routes>(routes{"10.0.0.2"}));
        if (--producers == 0)
        {
            updates.close();
        }
    };
    owner.spawn(consumer());
    for (int i = 1; i <= 3; ++i)
    {
        workers.context(static_cast<std::size_t>(i)).spawn(producer(i));
    }
    workers.run();
}
```

## 类型

### `overflow_policy`、`send_status`

```cpp
enum class overflow_policy
{
    reject,
    drop_oldest,
};

enum class send_status
{
    sent,
    dropped_oldest,
    full,
    closed,
};
```

| 值 | 含义 |
|:--|:--|
| `overflow_policy::reject` | 满的 channel 拒绝新值：`try_send` 返回 `full`；`send()` 和 `blocking_send()` 等待空位。默认值。 |
| `overflow_policy::drop_oldest` | 满的 channel 丢弃排队最久的值来腾出空位。任何发送都不会等待。 |
| `send_status::sent` | 值已入队，或直接交给了正在等待的 `receive()`。 |
| `send_status::dropped_oldest` | 丢弃了排队最久的值之后，值已入队。 |
| `send_status::full` | 未入队：channel 已满（仅在 `reject` 下的 `try_send`）。 |
| `send_status::closed` | 未入队：channel 已关闭。 |

### `channel`

```cpp
template <class T>
class channel
{
public:
    using value_type = T;

    channel(io_context& owner, std::size_t capacity, overflow_policy overflow = overflow_policy::reject);
    channel(const channel&) = default;
    channel& operator=(const channel&) = default;
    channel(channel&&) noexcept = default;
    channel& operator=(channel&&) noexcept = default;
    ~channel() = default;

    send_status try_send(T&& value);
    send_status try_send(const T& value);
    /* awaitable */ send(T value) noexcept;                     // co_await: send_status
    send_status blocking_send(T value, const cancellation_token& cancellation = {});
    /* awaitable */ receive() noexcept;                         // co_await: std::optional<T>
    std::optional<T> try_receive();
    void close() noexcept;
    bool closed() const;
    std::size_t size() const;
    std::size_t capacity() const noexcept;
};
```

有界的多生产者、单消费者队列。它的值由 `owner` 上的协程接收，任何地方都可以发送。`channel` 是句柄：副本共享
同一个队列，队列一直存活到最后一个副本销毁。`T` 必须能以 nothrow 方式移动构造和析构。

| 成员 | 行为 |
|:--|:--|
| `channel(owner, capacity, overflow)` | 容量为 `capacity` 的队列；所有槽位在此时分配。线程安全；`owner` 不必已在运行。容量为 0 时抛出 `std::invalid_argument`。在 `owner` 关停之后创建的 channel 一开始就是关闭的。 |
| `try_send(value)` | 可从任意线程调用，从不等待空位。返回 `sent`、`dropped_oldest`、`full` 或 `closed`。右值重载只在 `sent` 或 `dropped_oldest` 时才移走 `value`，因此被拒绝的值可以重试；`const T&` 重载发送一份副本。会在 owner 上恢复挂起的 `receive()`。 |
| `co_await send(value)` | 在任一正在运行的 context（包括 owner）上的协程里调用。在 `reject` 下，channel 满时发送方挂起，直到接收方腾出空位；等待的发送方按顺序得到服务，每个值被移入接收方腾出的槽位。返回 `sent`、`dropped_oldest` 或 `closed`；为 `closed` 时值被销毁。发送方在自己的 context 上恢复。 |
| `blocking_send(value, cancellation)` | 在不运行任何 PACE context 的线程上调用。与 `send()` 相同，但等待期间线程阻塞。`cancellation`（请求或截止时间）结束等待。 |
| `co_await receive()` | 只能在 owner 上调用。返回排队最久的值；channel 关闭且为空之后返回 `std::nullopt`。channel 为空时等待。同一时间只能有一个 receive。 |
| `try_receive()` | 只能在 owner 上调用。返回排队最久的值；channel 为空时返回 `std::nullopt`。owner 在 `stop()` 之后排空期间也能使用。 |
| `close()` | 可从任意线程调用，幂等。之后的发送返回 `closed`；等待的发送方以 `closed` 恢复；挂起的 receive 以 `std::nullopt` 恢复。已经入队的值仍可接收。 |
| `closed()`、`size()`、`capacity()` | 是否已关闭、排队值的数量（随时可能变化）以及容量。线程安全。 |

`try_send` 和 `send` 的开头会持有一个很短的内部锁；如果有 receive 在等待，就把它的恢复投递到 owner：它们从不等待
接收方。另一方已经在等待时，`send()` 和 `receive()` 直接交接值。

### `snapshot`

```cpp
template <class T>
class snapshot
{
public:
    explicit snapshot(runtime& workers, std::shared_ptr<const T> initial = nullptr);
    explicit snapshot(const std::vector<io_context*>& contexts, std::shared_ptr<const T> initial = nullptr);
    snapshot(const snapshot&) = default;
    snapshot& operator=(const snapshot&) = default;
    snapshot(snapshot&&) noexcept = default;
    snapshot& operator=(snapshot&&) noexcept = default;
    ~snapshot() = default;

    const std::shared_ptr<const T>& get() const;
    std::shared_ptr<const T> load() const;
    void publish(std::shared_ptr<const T> value);
};
```

以 RCU 风格把一个不可变值发布到一组固定的 context：一个 `runtime` 的全部 context，或列出的那些。每个 context 保存
自己缓存的 `std::shared_ptr<const T>`。`publish()` 存下新值，并请每个 context 在下一轮刷新自己的副本；读者从不等待
发布者，发布者也从不等待读者。`snapshot` 是句柄：副本共享值和缓存。

| 成员 | 行为 |
|:--|:--|
| `snapshot(workers, initial)` | 把 `initial`（可以为空）发布到 `workers` 的每个 context。线程安全；这些 context 不必已在运行。 |
| `snapshot(contexts, initial)` | 对列出的 context 做同样的事。列表为空、含空指针或重复的 context 时抛出 `std::invalid_argument`。 |
| `get()` | 在 snapshot 的某个 context 上调用：该 context 的副本，不加锁、没有原子读改写、不改变引用计数。引用在调用它的协程下一次挂起之前有效；要更久地持有值，就复制这个 `shared_ptr`。耗时与 context 数量成线性。在其他线程上抛出 `std::logic_error`。 |
| `load()` | 可从任意线程调用：最后发布的值，以新的 `shared_ptr` 返回。它会获取发布者的锁；在 context 上请优先用 `get()`。 |
| `publish(value)` | 可从任意线程调用：替换值。在 snapshot 的某个 context 上调用时，该 context 立即看到新值；其他每个 context 在它的下一轮之后看到。快速连续的多次发布可能合并为一次刷新到最新值。已停止的 context 不再刷新。 |

一个 context 看到的值按发布顺序出现。两个 context 可能短暂地不一致：一个还在读旧值，另一个已经在读新值。旧值在
丢掉它最后一个引用的 context 上释放。

## 函数

### `on`

```cpp
template <class Factory>
requires(std::is_invocable_v<Factory&, io_context&> || std::is_invocable_v<Factory&>)
auto on(io_context& target, Factory factory);   // task<operation_result<Factory>>

template <class T>
auto on(io_context& target, task<T> work);      // task<T>
```

在 `target` 上运行工作，然后带着结果或异常在等待它的协程自己的 context 上恢复。工厂（接受 `io_context&`，即
`target`，或者不接受参数）被移到 `target`，在那里调用，持有到工作结束，并在调用方恢复之前在那里销毁，和
[`task_group`](structured-concurrency.zh.md) 的子任务一样。`task` 重载把一个已经创建的 task 的帧移到 `target`；
task 借用的东西仍然是借用的。

工作在一个与调用方 token 相链接的 token 下运行，截止时间与调用方相同。调用方的取消、截止时间以及它所在 context
的 `stop()` 都会取消这项工作，而调用方总是等待工作结束：`on()` 从不丢下仍在运行的工作。在 `target` 自己上等待时，
`on()` 只是直接等待工厂返回的 task。

`on()` 返回惰性 task：在被等待之前，不会向 `target` 发送任何东西。请在正在运行的 context 上等待它。

## 错误

| 错误 | 时机 |
|:--|:--|
| `std::invalid_argument` | 容量为 0 的 `channel`；context 列表为空、含空指针或有重复的 `snapshot` |
| `std::logic_error` | 在 owner 之外调用 `receive()` 或 `try_receive()`，或同时进行第二个 `receive()`；在不运行 context 的线程上等待 `send()`；在运行 context 的线程上调用 `blocking_send()`；在不运行 snapshot 任何 context 的线程上调用 `snapshot::get()` |
| `std::system_error`，`operation_canceled` | 正在等待的 `send()` 或 `receive()` 的 task token 被取消或其 context 停止；owner 停止后调用 `receive()`；`blocking_send()` 的 token 被取消；`on()` 的 `target` 已停止（工作从未运行）、`target` 在工作结束前被销毁，或调用方的 token 或 context 取消了工作、工作随之结束 |
| `std::system_error`，`timed_out` | 上述等待因截止时间而结束 |
| 工作的异常 | `on()` 在调用方的 context 上重新抛出工作抛出的异常 |
| `send_status::closed` | 不是异常：发往已关闭 channel 的发送，包括因 owner 关停而关闭的 channel |

被取消的 `send()` 没有让它的值入队。返回 `sent` 的 `send()` 已让值入队，即使它的 token 恰好同时被取消；外层 task
的 awaiter 之后仍可能报告这次取消。

## 线程与并发

| 调用 | 调用方 |
|:--|:--|
| `channel` 构造函数、副本、`try_send`、`close`、`closed`、`size`、`capacity` | 任意线程 |
| `co_await channel::send` | 任一正在运行的 context 上的协程 |
| `channel::blocking_send` | 不运行 context 的线程 |
| `co_await channel::receive`、`channel::try_receive` | owner 的线程；同一时间一个 receive |
| `snapshot` 构造函数、副本、`load`、`publish` | 任意线程 |
| `snapshot::get` | 它的某个 context 的线程 |
| `co_await on(target, …)` | 任一正在运行的 context 上的协程；同时可以有任意多个，发往任意 target |

运行 context 的线程绝不能阻塞，所以不能使用 `blocking_send`。等待的发送方和调用方只挂起它们自己。

## 生命周期与所有权

- **owner 关停会关闭 channel。** owner 停止时（在它开始排空时；若从未运行，则在销毁时），channel 关闭：挂起的
  `receive()` 以 `operation_canceled` 失败，等待的发送方以 `closed` 恢复，阻塞的线程返回 `closed`，之后的发送返回
  `closed`。仍在排队的值随最后一个句柄销毁。`channel` 可以比它的 owner 活得更久，只是报告 `closed`。
- **句柄要比它的 co_await 活得久。** `send()` 和 `receive()` 通过调用它们的那个句柄引用 channel；在 `co_await`
  完成之前保持该句柄存活。其他句柄随时可以销毁。
- **被销毁的 context 带走它的等待者。** 挂起在 `send()`、`receive()` 或 `on()` 中的协程，如果它的 context 在恢复
  到达之前就完成了排空或被销毁，就会和该 context 其他挂起的帧一起被销毁，不再恢复。channel 或远端工作之后不会再碰它。
- **`on()` 的工作应拥有自己的数据。** 调用方会等待工作，所以在调用方的 context 运行期间，从调用方的帧借用是安全的。
  如果调用方的 context 停止，工作会被取消，但调用方可能在工作结束之前就随它的 context 一起被销毁：请按值捕获，
  或让共享数据随 target 一起存活。
- **snapshot 引用在下一次挂起时失效。** `get()` 返回指向该 context 缓存的引用，下一次刷新会在两轮之间替换它；
  要跨 `co_await` 持有值，就复制这个 `shared_ptr`。
- **`snapshot` 和 `channel` 只保存每个 context 的句柄**，从不保存 context 本身；它们引用的 context 可以先被销毁。

## 限制与默认值

| 项目 | 值 |
|:--|:--|
| channel 容量 | 至少为 1；全部 `capacity` 个槽位在构造时分配 |
| 默认溢出策略 | `overflow_policy::reject` |
| 每个 channel 挂起的 receive | 1 |
| 等待的发送方 | 不限；每个都在自己的协程帧或线程里等待 |
| 每个 snapshot context 待处理的刷新 | 最多 1 个；更多的发布合并到其中 |
| `snapshot::get()` 的开销 | 扫描一遍 snapshot 的 context |

## 参见

- [`runtime`](runtime.zh.md)：这些 context，以及按键选出 context 的 `shard_for`
- [`io_context`](io-context.zh.md)：`spawn` 和 `post`，更底层的跨线程入口
- [结构化并发](structured-concurrency.zh.md)：token、截止时间，以及 `on()` 为何像子任务
- [执行模型](../../explanation/execution-model.zh.md#放置工作)
