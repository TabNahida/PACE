<!-- source-hash: da0130ea14c6 -->
# 4. 任务与定时器

[English](04-tasks-and-timers.md) · [学习路线](README.zh.md) · [上一章：HTTP 服务器](03-http-server.zh.md)

学完本章，你会写出一个函数：它为定时器挂起，把值返回给 `main`，并通过普通的 C++ 异常报告失败。在 `hello` 项目中为新文件 `tasks.cpp` 添加一个目标：

```lua
target("tasks")
    set_kind("binary")
    add_files("tasks.cpp")
    add_deps("pace_runtime")
```

## 先返回一个值，再引入等待

返回 `pace::task<std::string>` 的协程最终会产出一个字符串。在协程里用 `co_return` 代替 `return`。调用协程只会创建它的任务，不会执行函数体：任务是惰性的。入口运行器会在调用线程上启动它。

把这个完整程序写入 `tasks.cpp`：

```cpp
#include <pace/runtime.hpp>
#include <chrono>
#include <iostream>
#include <string>

pace::task<std::string> greet(pace::io_context& io)
{
    co_await io.sleep_for(std::chrono::milliseconds(10));
    co_return "Hello, PACE";
}

int main()
{
    try
    {
        std::cout << pace::run(greet) << '\n';
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

运行 `xmake -y tasks` 和 `xmake run tasks`。预期输出 `Hello, PACE` 并正常退出。`greet` 等待定时器期间，事件循环可以执行其他就绪的工作；没有任何线程为这个协程睡眠。实际延迟可能超过 10 ms：定时器不提供实时保证。

`pace::run(greet)` 创建一个新的 `io_context`，在其上调用 `greet(io)` 并返回任务的结果。入口结束时，`run` 会停止该上下文，并等待已取消的后台工作展开完毕，所以这个一次性程序从不需要自己调用 `io.stop()`。对于返回 `task<void>` 的入口，`run` 返回 `void`。

## 传入自有的输入

要接受一个名字，把签名改为 `greet(pace::io_context& io, std::string name)`，并返回 `"Hello, " + name`。在等待期间，这个字符串归协程帧所有。用一个普通的工厂 lambda 传入额外参数：

```cpp
const auto message = pace::run([](pace::io_context& io) { return greet(io, "reader"); });
```

这个 lambda 返回一个任务，它本身不是协程。`run` 也能在整个运行期间安全地持有一个带捕获的协程 lambda：

```cpp
const auto answer = pace::run(
    [value = 42](pace::io_context& io) -> pace::task<int>
    {
        co_await io.yield();
        co_return value;
    }
);
```

这种生命周期保证只属于 `run`。不要把它推广到 `io.spawn([capture]() -> task<void> { ... }())`：立即调用的协程 lambda 是临时对象，它可能在协程仍引用其捕获时就被销毁。即使 `run` 持有闭包，引用捕获也仍然只是引用。

## 等待另一个任务

在协程中直接调用子任务：

```cpp
const auto message = co_await greet(io, "reader");
```

子任务的结果就绪后，父协程才恢复。如果先把任务保存起来，要用 `co_await std::move(pending)` 消费它：任务只能移动，且只能被等待一次。子任务的异常会在 `co_await` 表达式处抛出。父协程能恢复时就在那里捕获；否则它会传到 `run`，再到 `main` 里的 `catch`。

`co_await io.yield()` 和零时长定时器都会让其他就绪工作先执行一轮，但都不会把 CPU 工作移到别的线程。阻塞的文件读取或耗时计算仍会阻塞这个事件循环；[第 8 章](08-workers-and-threads.zh.md)说明这类工作该放在哪里。

## 理解结束时谁拥有什么

`run` 返回**自有的结果**，例如字符串或数字。不要返回绑定在其临时上下文上的 socket，也不要返回指向入口或其捕获所拥有存储的视图。

入口可以用 `io.spawn` 启动后台工作，但入口结束会取消这些工作；`run` 不会等它们自然完成。后台任务必须拥有自己的数据，或者只借用在 `run` 之外声明、存活到 `run` 返回的对象。入口的局部变量会在入口返回时销毁，此时后台任务未必已经观察到取消。如果程序需要多个独立的根任务并显式排空，请使用下面的手动模式以及[第 9 章](09-graceful-shutdown.zh.md)。

## 需要时显式掌控

```cpp
pace::io_context io;
io.spawn(service(io)); // service returns task<void>
io.run();              // Returns only after someone calls io.stop().
```

与 `pace::run(factory)` 不同，`io.run()` 在最后一个任务结束后仍会继续运行，适合从其他线程接收工作的服务。选择其中一种形式之前，先确定由谁负责关闭。两种形式都不能在已经运行着事件循环的线程上再嵌套运行一个。`pace::run` 的入口被取消时，`run` 会以 `std::system_error`（错误码 `std::errc::operation_canceled`）报告，而不是返回一个值。

试着让入口在定时器之后抛出异常，确认 `main` 报告了失败并退出，而不是挂住。然后运行配套程序 `xmake run tutorial_tasks`（[源码](../../examples/tutorial_tasks.cpp)）：它的 `lesson` 协程还会等待一个先保存起来的任务，检查自己仍在同一个上下文上运行，让出执行，并捕获子任务的异常。

完整约定见 [`pace::run`](../reference/runtime/run.zh.md) 和 [`task`](../reference/runtime/task.zh.md)。

下一章：[5. TCP](05-tcp.zh.md)
