<!-- source-hash: f1394cd7ba2e -->
# 9. 有序关闭

[English](09-graceful-shutdown.md) · [学习路线](README.zh.md) · [上一章：工作线程与线程](08-workers-and-threads.zh.md)

编写长期运行的服务器之前，先做一个小小的取消实验。把这个完整程序保存为 `shutdown.cpp`，添加一个依赖 `pace_runtime` 的 `shutdown` 目标，然后构建并运行：

```cpp
#include <pace/runtime.hpp>
#include <chrono>
#include <iostream>

pace::task<void> waiting(pace::io_context& io, bool& canceled)
{
    try
    {
        co_await io.sleep_for(std::chrono::hours(1));
    }
    catch (const std::system_error& error)
    {
        if (error.code() != std::errc::operation_canceled)
        {
            throw;
        }
        canceled = true;
    }
}

int main()
{
    try
    {
        pace::io_context io;
        bool canceled = false;
        auto stop = io.call_after(std::chrono::milliseconds(10), [&io] { io.stop(); });
        io.spawn(waiting(io, canceled));
        io.run();
        if (!canceled)
        {
            throw std::runtime_error("cancellation was not observed");
        }
        std::cout << "Canceled and drained\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

预期输出 `Canceled and drained`，而不必等上一小时。定时器回调停止上下文；等待中的协程随后带着取消错误恢复，只有在它展开完毕后，`io.run()` 才返回。要把定时器句柄保存在变量中：销毁它会解除回调。`canceled` 位于任务之外，所以在取消期间它仍然存活。[第 4 章](04-tasks-and-timers.zh.md)的入口运行器在入口完成时执行的正是这种停止；手动运行的服务则自己选择停止的时机。

配套程序 [`tutorial_shutdown.cpp`](../../examples/tutorial_shutdown.cpp) 做同样的事，由另一个任务在 5 ms 后停止上下文。`xmake run tutorial_shutdown` 检查取消已被观察到，并且协程中的局部 RAII 守卫在 `run()` 返回之前已经执行，然后输出 `Cancellation observed; coroutine resources released`。

对于单个操作或一组相关任务，优先使用[第 7 章](07-structured-concurrency.zh.md)的局部取消和任务组：`io.stop()` 会关闭整个上下文。关闭监听器只会取消它挂起的 accept；HTTP 的 `serve` 函数在结构化作用域中运行各个连接，返回之前会取消并汇合它们。

## 取消是一条普通的错误路径

```cpp
try
{
    co_await io.sleep_for(std::chrono::hours(1));
}
catch (const std::system_error& error)
{
    if (error.code() != std::errc::operation_canceled)
    {
        throw;
    }
    // Release synchronous application resources and finish this task.
}
```

`stop()` 永久地请求关闭并唤醒事件循环。挂起的 socket 等待和定时器等待以取消恢复；已经就绪的任务和回调仍会执行。协程帧中的 RAII 对象随着协程展开而销毁。已停止的上下文拒绝新的 `spawn` 和 `post` 调用，之后的每次等待也都会看到取消。任何异步刷新都要在停止上下文**之前**完成。

上下文正在停止时，以未捕获的取消结束的根任务不算错误。根任务的其他失败仍会在排空完成后从 `run()` 传出。从不挂起或从不返回的代码，以及阻塞在同步调用中的代码，都可能让关闭无法完成：停止不会中断线程。

## 先停止接受连接，再停止事件循环

对于需要完成已开始响应的服务，按顺序执行以下步骤：

1. 在所属上下文上把应用标记为排空中，并关闭监听器。
2. 让已接受的连接完成；在合适的情况下，让处理函数返回 `close = true` 的响应。
3. 等待活跃连接计数降到零，最长等到应用自己选定的宽限期。
4. 调用 `io.stop()` 或 `runtime.stop()`，取消剩下的一切。
5. 等待 `run` 返回并汇合运行它的线程，销毁 socket 和监听器，最后销毁上下文。

关闭监听器只会取消挂起的 accept；已接受的流继续运行。接受循环必须捕获这个预期中的取消并返回，[第 5 章](05-tcp.zh.md)的配套程序就是这样做的。否则，在上下文停止**之前**以未捕获的取消结束的根任务会被当作失败，过早地停止事件循环。`http::serve` 已经以这种方式处理监听器关闭。

下面的辅助函数假设 `active_connections` 只在同一个上下文上修改，并且每个连接任务都通过 RAII 守卫递减它：

```cpp
pace::task<void> drain_connections(
    pace::io_context& io,
    pace::tcp::listener& listener,
    const std::size_t& active_connections,
    std::chrono::milliseconds grace
)
{
    listener.close();
    const auto deadline = std::chrono::steady_clock::now() + grace;
    while (active_connections != 0 && std::chrono::steady_clock::now() < deadline)
    {
        co_await io.sleep_for(std::chrono::milliseconds(1));
    }
    io.stop();
}
```

这是应用层的模式，不是库提供的连接计数 API。监听器、计数和上下文都必须比这个辅助函数和各连接任务活得更久；请选择有界的宽限期。`http::serve` 不提供活跃连接计数，也不提供汇合句柄。如果需要，请自己写接受循环，在一个负责计数的协程中运行 `http::serve_connection`。有多个工作线程时，通过各自的上下文分别排空；跨线程共享的计数必须同步。

## socket 关闭与对象生命周期

`socket.close()` 取消 socket 已登记的等待，并在所属事件循环上释放原生句柄。它不会等待挂起的操作结束。要让 C++ socket 对象一直存活到这些操作恢复并展开：先销毁或移动它，会让那些成员协程持有悬空的 `this`。不要用销毁挂起任务的方式来模拟取消。

`co_await socket.close_gracefully()` 是另一种操作。在 socket 上没有其他未完成操作时，它关闭发送方向，默认在最多 100 ms 或 64 KiB 的范围内丢弃对端的输入，然后关闭。即使出错或被取消，它也总会关闭 socket。请在最后一次写入之后、停止上下文之前使用它。它限制的是排空过程，而不是对端的确认；达到限制属于正常完成。

## 信号与外部所有者

服务通常由外部停止：Ctrl-C 发送 `SIGINT`，服务管理器发送 `SIGTERM`，Windows 上则是控制台的 Ctrl-C 和关闭事件。
`pace::wait_for_signal` 把它们变成协程可以等待的值，于是在拥有这些工作的任务里，关闭只需两行：

```cpp
pace::task<void> serve_until_signal(pace::tcp::listener& listener)
{
    co_await pace::with_task_group(
        [&](pace::task_group& group) -> pace::task<void>
        {
            (void)group.spawn(accept_loop(listener)); // 第 5 章中你自己的 accept 循环
            co_await pace::wait_for_signal({pace::process_signal::interrupt, pace::process_signal::terminate});
            group.cancel();
        }
    );
}
```

`wait_for_signal` 在被调用时就完成订阅。从那以后这些信号归 PACE 处理：原本会结束进程的默认动作不再执行，信号的递送在
协程自己的上下文上恢复它。`group.cancel()` 取消 accept 循环以及组里的其他一切，任务组在作用域返回之前汇合它们。若要先
排空连接，就在这里执行上面 `drain_connections` 的步骤，而不是立即取消。最后一个订阅消失后，这些信号先前的处置会恢复，
所以关闭开始之后再按一次 Ctrl-C，进程会照常结束。

配套服务器（`tutorial_http serve`、`tutorial_tcp serve`、`tutorial_websocket serve`）就是这样停止各自的上下文的。需要
多次响应同一个信号的程序，例如用 `SIGHUP` 重新加载配置，应保留一个 `pace::signal_set`，在循环中等待 `set.wait()`；两次
等待之间到达的递送不会丢失。Windows 服务收不到控制台事件：它的控制处理函数调用
`pace::deliver_signal(pace::process_signal::terminate)`。不要自己编写调用反应器或 socket 函数的信号处理函数；在另一个普通
线程上，调用 `stop()` 本身是安全的。

`runtime.run()` 和 `runtime.run_on_current_thread()` 在返回之前汇合各自的工作线程。如果你在应用自己拥有的线程上调用它们，仍然需要在销毁 runtime 之前汇合那个线程。你自己运行的 `io_context` 同样必须先停止，再汇合运行它的线程。也要汇合你自己的阻塞工作线程，以免它们向已销毁的上下文提交结果。

参考：[`io_context`](../reference/runtime/io-context.zh.md)、[`runtime`](../reference/runtime/runtime.zh.md)、[句柄与信号](../reference/runtime/handles-and-signals.zh.md)、[TCP `close_gracefully`](../reference/net/tcp.zh.md)。

下一章：[10. TLS 与 DNS](10-tls-and-dns.zh.md)
