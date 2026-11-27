<!-- source-hash: 84bbb82e24f9 -->
# 8. 工作线程与线程

[English](08-workers-and-threads.md) · [学习路线](README.zh.md) · [上一章：结构化并发](07-structured-concurrency.zh.md)

到目前为止，一个线程只运行一个事件循环。本章把独立的工作交给两个事件循环，再在应用自己拥有的线程上运行事件循环。把这个完整程序保存为 `workers.cpp`，并添加一个依赖 `pace_runtime` 的 `workers` 目标：

```cpp
#include <pace/runtime.hpp>
#include <atomic>
#include <chrono>
#include <iostream>

pace::task<void> work(pace::runtime& pool, std::size_t index, std::atomic<unsigned>& remaining)
{
    auto& io = pool.context(index);
    co_await io.sleep_for(std::chrono::milliseconds(5));
    if (remaining.fetch_sub(1) == 1)
    {
        pool.stop();
    }
}

int main()
{
    try
    {
        pace::runtime pool(2);
        std::atomic<unsigned> remaining{2};
        pool.context(0).spawn(work(pool, 0, remaining));
        pool.context(1).spawn(work(pool, 1, remaining));
        pool.run_on_current_thread();
        std::cout << "Both workers finished\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

构建并运行 `workers`，预期输出 `Both workers finished`。计数器是原子的，因为两个任务可能在不同的操作系统线程上访问它。每个任务在启动之前就被放到自己的上下文上，`co_await` 永远不会把它移到另一个上下文。最后完成的任务停止两个上下文，`run_on_current_thread` 在 `remaining` 和 `pool` 销毁之前汇合工作线程。把 `run_on_current_thread()` 改成 `run()`，两个上下文都会在后台线程上运行，结果相同。

配套程序 [`tutorial_workers.cpp`](../../examples/tutorial_workers.cpp) 以两种模式运行同样的布局，并检查每个上下文由哪个线程执行：

```sh
xmake run tutorial_workers
xmake run tutorial_workers background
```

两种模式都使用两个上下文，并检查每个任务都留在分配给它的上下文上。默认的 `caller` 模式还会检查上下文 0 运行在 `main` 的线程上。最后完成的任务停止 runtime，程序输出 `Two contexts completed in caller mode`（或 `background mode`）。

## 选择执行布局

```cpp
pace::runtime runtime(2);
// Spawn work on runtime.context(0) and runtime.context(1).
runtime.run_on_current_thread();
```

这会创建一个后台工作线程，另一个上下文使用调用线程。参数 `2` 是**上下文的总数**，包括调用者的那个。`runtime(1).run_on_current_thread()` 不创建任何工作线程。如果调用线程只需要等待，就使用 `runtime.run()`：它为每个上下文启动一个线程并汇合全部线程。两种调用都会阻塞，直到 runtime 被停止并排空；都不会留下在后台运行的服务。

不传数量时，构造函数使用报告的硬件并发数，至少为一。你可以传入资源允许的任意正数。工作通过 `context(index)` 放到你指定的位置：上下文之间不做负载均衡，也不做任务窃取。在上下文之间共享的可变对象需要普通的 C++ 同步，就像上面的原子计数器。

不论选择哪种模式，runtime 都只能运行一次；再次调用任一 run 方法都是错误。调用者模式不能在另一个正在运行的 PACE 上下文中使用。某个根任务失败时，runtime 会停止其他上下文，并在汇合工作线程后重新抛出该失败。

## 不用 runtime 运行

[第 4 章](04-tasks-and-timers.zh.md)介绍了手动模式：一个上下文，在调用线程上执行 `io.run()`。上下文也可以运行在应用自己拥有的线程上。在 [`tutorial_tasks.cpp`](../../examples/tutorial_tasks.cpp) 的副本中，在 `lesson` 之后添加这个协程，让上下文有办法停止：

```cpp
pace::task<void> controlled_lesson(pace::io_context& io)
{
    co_await lesson(io);
    io.stop();
}
```

然后把 `main` 中 `try` 块的内容替换为：

```cpp
pace::io_context io;
std::exception_ptr failure;
io.spawn(controlled_lesson(io));
std::thread run_thread(
    [&io, &failure]
    {
        try
        {
            io.run();
        }
        catch (...)
        {
            failure = std::current_exception();
        }
    }
);
run_thread.join(); // controlled_lesson() stops the context; join also makes failure safe to read.
if (failure)
{
    std::rethrow_exception(failure);
}
return 0;
```

包含 `<thread>` 和 `<exception>`。这个 lambda 是普通的线程函数，不是协程，它引用的两个对象都活得比汇合更久。长期运行的服务会先在控制线程上调用 `io.stop()`，再汇合。要确保异常展开 `main` 时也会执行汇合：销毁一个仍可汇合的 `std::thread` 会终止进程。销毁 `io` 不会替你汇合任何线程。

你可以创建多个独立的上下文，各自运行在一个应用线程上。绝不要让一个上下文同时在多个线程上运行，也不要在同一线程上于另一个 PACE 上下文内部启动 `run`。

## 从其他线程提交工作

`io.spawn(std::move(task))` 移交一个自有的根任务，`io.post(function)` 排队一个普通回调。两者都可以从任何线程安全调用。回调可以修改事件循环拥有的状态、关闭仍然存活的 socket，或在该事件循环上启动另一个协程。回调引用的一切都必须存活到它执行为止；上下文停止后，提交会以 `std::errc::operation_canceled` 被拒绝。

```cpp
io.post([&io] { io.stop(); });
```

这个回调引用的上下文，其生命周期由它的所有者控制。要传递数据，按值捕获一个自有的 `std::string` 或 `shared_ptr`。`post` 接受 `std::function`，在 C++20 中它要求可复制的可调用对象：只持有 `unique_ptr` 的 lambda 不适用。传给 `spawn` 的任务则可以在协程帧中持有只能移动的值。

## 让阻塞工作远离事件循环

协程中的同步文件系统调用、数据库驱动或耗时计算，仍会阻塞执行它的线程。PACE 没有用于阻塞工作的可等待执行器。请在应用自己拥有的线程上执行这类工作，再把自有的结果 `post` 回需要它的上下文。在这些线程汇合之前保持上下文存活，并处理关闭期间被拒绝的 `post`。

把 `runtime(1)` 改成 `runtime(8)` 并不会让上下文 0 上的阻塞调用并行执行：它只是多创建了七个独立的事件循环。`co_await io.yield()` 和等待子任务都不会把计算移到其他线程。

如何选择工作线程数量、跨工作线程共享端口的监听器以及如何衡量效果，见[协议调优](../guides/protocol-tuning.zh.md)。参考：[`runtime`](../reference/runtime/runtime.zh.md)、[`io_context`](../reference/runtime/io-context.zh.md)。

下一章：[9. 有序关闭](09-graceful-shutdown.zh.md)
