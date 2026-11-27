<!-- source-hash: 3ec0d05d446e -->
# `pace::runtime`

[English](runtime.md) · [运行时](README.zh.md)

## 概述

`runtime` 持有固定数量、彼此独立的 `io_context`，并让每个 context 在自己的线程上运行。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/runtime/runtime.hpp>`（也可用 `<pace/runtime.hpp>`） | `pace_runtime` | 始终构建 |

这些 context 是分片而不是线程池：task 留在应用交给它的那个 context 上。没有工作窃取，不会自动分发 task，
不绑定 CPU，也没有单独的阻塞工作池。

## 概要

```cpp
#include <pace/runtime.hpp>

#include <atomic>
#include <chrono>

using namespace std::chrono_literals;

pace::task<void> work(pace::runtime& workers, std::atomic<int>& done)
{
    co_await pace::sleep_for(5ms); // 在 spawn 它的 context 上运行
    if (++done == static_cast<int>(workers.size()))
    {
        workers.stop();
    }
}

int main()
{
    pace::runtime workers(4);
    std::atomic<int> done{0};
    for (std::size_t i = 0; i < workers.size(); ++i)
    {
        workers.context(i).spawn(work(workers, done));
    }
    workers.run_on_current_thread(); // context 0 在本线程，context 1-3 在新线程
}
```

## 类型

### `runtime`

```cpp
class runtime
{
public:
    explicit runtime(
        std::size_t workers = std::max(1u, std::thread::hardware_concurrency()),
        io_backend backend = io_backend::automatic
    );
    ~runtime();
    runtime(const runtime&) = delete;
    runtime& operator=(const runtime&) = delete;

    std::size_t size() const noexcept;
    io_context& context(std::size_t index);
    template <class Key>
    std::size_t shard_index(const Key& key) const;
    template <class Key>
    io_context& shard_for(const Key& key);
    void run();
    void run_on_current_thread();
    void stop() noexcept;
};
```

| 成员 | 行为 |
|:--|:--|
| `runtime(workers, backend)` | 创建 `workers` 个使用 `backend` 的 context，但不创建线程。默认值是报告的硬件并发数，未知时为 1。为 0 时抛出 `std::invalid_argument`，也会传播 `io_context` 构造函数抛出的异常。 |
| `~runtime()` | 销毁各个 context。`run()` 或 `run_on_current_thread()` 必须已经返回。从未运行过的 context 上仍在排队的工作会被销毁而不执行。 |
| `size()` | context 数量，构造时确定。 |
| `context(index)` | 位于 `index` 的 context；`index >= size()` 时抛出 `std::out_of_range`。其地址在 runtime 生命周期内保持不变。用 `context(i).spawn(…)` 或 `context(i).post(…)` 提交工作。 |
| `shard_index(key)` | 拥有 `key` 的 context 的下标：对键做 `std::hash<Key>`，再混合并对 `size()` 取模。同一个键在 runtime 生命周期内总是得到同一个下标；跨进程或跨构建时，它的稳定性与 `std::hash` 相同。线程安全。 |
| `shard_for(key)` | `context(shard_index(key))`：`key` 背后状态的固定属主，例如为该键持有 [`channel`](cross-context.zh.md#channel) 的 context。线程安全。 |
| `run()` | 为每个 context 启动一个线程，阻塞到它们全部停止并被 join。 |
| `run_on_current_thread()` | 在调用线程上运行 context 0，其余 context 运行在 `size() - 1` 个新线程上；阻塞方式相同。只有一个 context 时不创建线程。 |
| `stop()` | 请求所有 context 停止。线程安全、幂等、不等待。 |

两个运行函数的区别只在于 context 0 运行在哪里：

| 调用（`N = size()`） | 调用线程 | runtime 创建的线程 |
|:--|:--|:--|
| `run()` | 等待，然后 join 工作线程 | N |
| `run_on_current_thread()` | 运行 context 0，然后 join 其余线程 | N − 1 |

## 错误

| 错误 | 时机 |
|:--|:--|
| `std::invalid_argument` | 以 0 个 worker 构造 |
| `std::out_of_range` | `context(index)` 中 `index >= size()` |
| `std::logic_error` | 第二次调用 `run()` 或 `run_on_current_thread()`（runtime 只能运行一次，无论哪种模式）；在已运行某个 context 的线程上调用 `run_on_current_thread()`。这种嵌套调用在启动任何东西之前就被拒绝，不会消耗 runtime 唯一的一次运行。 |
| 第一个 context 错误 | 任一 context 的 `run()` 逃出的异常（见 [`io_context`](io-context.zh.md#错误)）会停止所有 context；所有线程 join 之后，重新抛出第一个异常。 |
| `std::system_error` | 创建线程失败：已启动的 context 被停止并 join，然后传播该错误。 |

每个 context 也只能运行一次：先自己运行某个 context，再运行 runtime，会使该 context 的 `run()` 失败，进而停止其余 context。

## 线程与并发

`stop()`、`shard_index`、`shard_for` 以及任一 `context(i)` 上的 `spawn`/`post`/`stop` 可以从任何线程调用，包括 runtime 自己的 context 上的
task 或回调。关于 context、其 socket 和 task 的其他一切都遵循 [`io_context` 的规则](io-context.zh.md#线程与并发)：
每个 context 一个线程，在 context 之间共享的对象需要应用自己同步。

这些线程是普通的 `std::thread`，不绑定 CPU。阻塞的 task 只占住自己的 context。每个逻辑 CPU 一个 context
只是起点，不是规则：要给客户端、阻塞工作和其他服务留出 CPU，并按实际连接数测量。连接很少时，
把它们分散到更多 context 上可能让每次轮询只有一个就绪描述符；请在多个 worker 数量下比较吞吐量和尾延迟。

## 生命周期与所有权

- runtime 持有它的 context；`context(i)` 返回的引用在 runtime 销毁前一直有效。
- `run()` 和 `run_on_current_thread()` 只有在所有线程都 join 之后才返回。在此之后、销毁 runtime 之前，
  销毁绑定到这些 context 的 socket 和 listener。
- 析构函数不会与仍在 `run()` 中的线程协调：不要销毁正在运行的 runtime。
- 已停止的 runtime 不能重新启动。

## 限制与默认值

| 项目 | 值 |
|:--|:--|
| 默认 `workers` | `std::thread::hardware_concurrency()`，至少为 1 |
| 默认 `backend` | `io_backend::automatic` |
| 最小 `workers` | 1 |
| 每个 runtime 的运行次数 | 1，无论哪种模式 |

## 参见

- [`io_context`](io-context.zh.md)：每个 context 的职责
- [跨 context 通信](cross-context.zh.md)：这些 context 之间的 channel、snapshot 和 `on()`
- [I/O 后端](io-backends.zh.md)：`backend` 参数
- [教程：工作线程](../../tutorial/08-workers-and-threads.zh.md)
- [运行时性能](../../performance/runtime.zh.md)
- [执行模型](../../explanation/execution-model.zh.md)
