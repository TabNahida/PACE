# 12. 一起启动任务，一起等待结束

[English](12-structured-concurrency.md) · [学习路线](README.zh-CN.md) · [参考](../reference/structured-concurrency.zh-CN.md)

单次请求仍然直接写 `http::get(url)`。多个异步操作需要共享生命周期时，再使用任务组。
本章使用当前源码并链接 `pace_runtime`，这些新增接口尚未发布。

## 1. 启动两个任务并取得结果

在第一章的工程里将依赖改为 `pace_runtime`，把以下完整程序保存为 `main.cpp`：

```cpp
#include <pace/task_group.hpp>
#include <chrono>
#include <iostream>
#include <string>
using namespace std::chrono_literals;

pace::task<std::string> main_task() {
    co_return co_await pace::with_task_group([](pace::task_group& group) -> pace::task<std::string> {
        auto first = group.spawn([]() -> pace::task<std::string> {
            co_await pace::sleep_for(10ms);
            co_return "Hello";
        });
        auto second = group.spawn([]() -> pace::task<std::string> {
            co_await pace::sleep_for(5ms);
            co_return "structured PACE";
        });
        auto a = co_await first;
        auto b = co_await second;
        co_return a + ", " + b + "!";
    }, {.deadline = pace::deadline::after(2s)});
}
int main() {
    try { std::cout << pace::run(main_task) << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
```

构建并运行 `my_application`，应输出 `Hello, structured PACE!` 后退出。
第二个任务可以先完成，但每个 handle 始终对应自己的结果。
`group.spawn` 在当前 context 启动子任务，`co_await first` 消费一次结果。
即使丢弃 handle，任务组仍然会等待对应子任务。

`with_task_group` 在主体和所有子任务结束后才返回。子任务失败时，先取消并等待兄弟任务，
再向调用方传播异常；主体抛异常时也会取消并等待子任务。主体正常返回时等待剩余任务，
不会主动取消它们。

两秒 deadline 覆盖整个任务组和嵌套调用。它使用单调时钟的绝对时间点，内层操作不能
重新计时或延长预算。超时在清理完成后抛出 `system_error(errc::timed_out)`。

## 2. 把定时器替换成网络请求

包含 `<pace/http.hpp>` 并链接 `pace_http1`，保持快速入门的服务端运行，将任务组主体换成：

```cpp
[](pace::task_group& group) -> pace::task<std::string> {
    auto first = group.spawn(pace::http::async_get("http://127.0.0.1:8080/health"));
    auto second = group.spawn(pace::http::async_get("http://127.0.0.1:8080/health"));
    auto a = co_await first;
    auto b = co_await second;
    co_return a.body + b.body;
}
```

两个请求使用独立连接，继承任务组的取消和 deadline。启用对应模块并准备匹配的服务端后，
`http2::async_get`、`http3::async_get` 使用相同写法，见[统一 HTTP 客户端](../reference/http-clients.zh-CN.md)。

## 3. 只取消一个操作，不停止事件循环

在协程里，可以给一次调用单独设置预算：

```cpp
try {
    auto reply = co_await pace::with_deadline(pace::deadline::after(100ms),
        pace::http::async_get("http://127.0.0.1:8080/health"));
    std::cout << reply.body;
} catch (const std::system_error& error) {
    if (error.code() != std::errc::timed_out) throw;
    std::cout << "request timed out\n";
}
```

同一事件循环上的其他任务继续运行。希望兄弟任务继续执行时，在子任务内部处理可恢复错误；
异常离开子任务就会使整个任务组失败。
外部取消使用 `pace::cancellation_source`：通过 `with_cancellation` 或 `operation_options`
传入 `source.token()`，控制线程调用 `source.request_cancellation()`，得到
`errc::operation_canceled`。只有关闭整个事件循环时才需要 `io.stop()`。

## 4. 给共享数据一个可靠的所有者

子任务工厂持有自己的值捕获，直到子任务完成。它可以借用整个 `with_task_group` 调用外部的
调用方局部变量，但不能借用在主体返回或抛异常时就销毁的主体局部变量。可使用 `group.own`：

```cpp
co_await pace::with_task_group([](pace::task_group& group) -> pace::task<void> {
    auto& text = group.own(std::string("owned until every child finishes"));
    (void)group.spawn([&text]() -> pace::task<void> {
        co_await pace::sleep_for(5ms);
        std::cout << text << '\n';
    });
    co_return; // 子任务等待期间 text 仍然存活
});
```

`group.own` 保证对象在所有子任务收尾后才销毁，异常路径也一样。
也可以把数据复制或移动到子任务工厂。带捕获的协程 lambda 应直接传给 `spawn`，
不要立即调用临时 lambda 后再传入借用该临时对象的 task。不要返回借用组内存储的 socket 或 view。

仓库配套程序为 `xmake run tutorial_structured`。清理时的 shield、错误优先级、自定义 awaiter
和并发约定放在[参考文档](../reference/structured-concurrency.zh-CN.md)。
