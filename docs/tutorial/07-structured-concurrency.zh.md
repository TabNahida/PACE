<!-- source-hash: 5a7270b75ee5 -->
# 7. 结构化并发

[English](07-structured-concurrency.md) · [学习路线](README.zh.md) · [上一章：UDP](06-udp.zh.md)

要拿到一个响应，直接调用 `http::get(url)` 仍然最简单。当多个异步操作必须一起开始、一起结束时，就把它们放进一个任务组。本章链接 `pace_runtime`，从第二节起改为链接 `pace_http1`。

## 启动两个任务并收集结果

把这个完整程序保存为 `structured.cpp`，并添加一个依赖 `pace_runtime` 的 `structured` 目标。它就是配套程序 [`tutorial_structured.cpp`](../../examples/tutorial_structured.cpp)：

```cpp
#include <pace/runtime.hpp>

#include <chrono>
#include <iostream>
#include <string>
using namespace std::chrono_literals;

pace::task<std::string> main_task()
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
            auto a = co_await first;
            auto b = co_await second;
            co_return a + ", " + b + "!";
        },
        {.deadline = pace::deadline::after(2s)}
    );
}

int main()
{
    try
    {
        std::cout << pace::run(main_task) << '\n';
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

构建并运行 `structured`，或者在 PACE 源码目录中运行 `xmake run tutorial_structured`。预期输出 `Hello, structured PACE!` 并正常退出。第二个任务先完成，但每个句柄给出的都是它自己那个任务的结果。`group.spawn` 在当前上下文上启动一个子任务，`co_await first` 取走该子任务的结果，且只能取一次。即使丢弃了句柄，子任务仍属于任务组，任务组会等待它。

`with_task_group` 只有在函数体和所有子任务都结束后才返回。某个子任务失败时，它的兄弟任务会先被取消并汇合，异常才传给调用者。函数体抛出异常时，子任务同样会被取消并汇合。函数体正常返回时，任务组会等待剩余的子任务，而不会取消它们。

两秒的截止时间覆盖整个任务组，包括嵌套调用。它是单调时钟上的一个绝对时间点，嵌套操作无法重新计时或延长它。截止时间到达时，任务组取消其中的工作，清理完成后抛出 `std::system_error`，错误码为 `std::errc::timed_out`。

## 用网络请求替换定时器

包含 `<pace/http1.hpp>`，并把目标的依赖改为 `pace_http1`。在[第 1 章](01-install-and-first-program.zh.md)的 `hello` 服务器运行时，把任务组的函数体换成：

```cpp
[](pace::task_group& group) -> pace::task<std::string>
{
    auto first = group.spawn(pace::http::async_get("http://127.0.0.1:8080/health"));
    auto second = group.spawn(pace::http::async_get("http://127.0.0.1:8080/health"));
    auto a = co_await first;
    auto b = co_await second;
    co_return a.body + b.body;
}
```

预期输出两次 `ok`。两个请求各用自己的连接，并继承任务组的取消和截止时间。启用相应模块且服务器支持对应协议时，同样的代码也适用于 `http2::async_get` 或 `http3::async_get`；见 [URL 客户端](../reference/http/clients.zh.md)。

## 只取消一个操作，不停止事件循环

在任务内部，可以给单个操作设定自己的预算：

```cpp
try
{
    auto reply = co_await pace::with_deadline(
        pace::deadline::after(100ms),
        pace::http::async_get("http://127.0.0.1:8080/health")
    );
    std::cout << reply.body;
}
catch (const std::system_error& error)
{
    if (error.code() != std::errc::timed_out)
    {
        throw;
    }
    std::cout << "request timed out\n";
}
```

同一事件循环上的其他任务继续运行。如果希望兄弟任务继续，请在子任务内部处理可以恢复的错误：逃出子任务的错误会让整个任务组失败。要从外部取消，创建一个 `pace::cancellation_source`，把 `source.token()` 传给 `with_cancellation`（或放进 `operation_options`），再由控制方调用 `source.request_cancellation()`。被取消的操作抛出 `std::system_error`，错误码为 `std::errc::operation_canceled`。只有要关闭整个事件循环时才使用 `io.stop()`。

## 为共享数据找一个安全的所有者

子任务的工厂拥有它捕获的值，直到子任务完成。子任务可以借用调用者在整个 `with_task_group` 调用期间都保持存活的数据，但不能借用函数体的局部变量：函数体返回或抛出异常时，其局部变量就被销毁，而子任务可能仍在运行。这类数据要交给任务组，使用 `group.own`：

```cpp
co_await pace::with_task_group(
    [](pace::task_group& group) -> pace::task<void>
    {
        auto& text = group.own(std::string("owned until every child finishes"));
        (void)group.spawn(
            [&text]() -> pace::task<void>
            {
                co_await pace::sleep_for(5ms);
                std::cout << text << '\n';
            }
        );
        co_return; // text stays alive while the child waits
    }
);
```

`group.own` 把对象保留到所有子任务汇合为止，异常展开任务组时也是如此。把数据复制或移动进子任务的工厂同样可行。要把带捕获的协程 lambda 本身传给 `spawn`；不要先调用一个临时 lambda 再传入它返回的任务，那个任务会借用已销毁的 lambda。不要返回借用任务组所拥有存储的 socket 或视图。

清理时屏蔽取消、多个失败时哪个错误胜出，以及线程规则，见[结构化并发参考](../reference/runtime/structured-concurrency.zh.md)。

下一章：[8. 工作线程与线程](08-workers-and-threads.zh.md)
