# 2. 写出第一个异步程序

[English](02-tasks-timers.md) · [上一章：构建](01-build.zh-CN.md) · [下一章：TCP](03-tcp.zh-CN.md)

这一章从“等待一下，再返回字符串”开始。完成后你会知道任务何时执行、
`co_await` 暂停了谁，以及结果和错误怎样回到 `main`。
使用第一章的 `my_application` target，依赖 `pace_runtime`。

## 第一步：写一个会返回值的任务

把下面的完整程序写入 `main.cpp`：

```cpp
#include <pace/run.hpp>
#include <chrono>
#include <iostream>
#include <string>

pace::task<std::string> greet(pace::io_context& io) {
    co_await io.sleep_for(std::chrono::milliseconds(10));
    co_return "Hello, PACE";
}

int main() {
    try {
        std::cout << pace::run(greet) << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

运行 `xmake -y my_application`，然后 `xmake run my_application`。
预期输出 `Hello, PACE`，程序随即正常退出。

`task<std::string>` 表示这个函数最终产生一个字符串；`co_return` 交出结果。
`co_await` 暂停的是当前协程，事件循环可以继续处理别的就绪工作，
并没有为这个等待单独创建一个睡眠线程。定时器到期只代表可以恢复，实际恢复可能更晚。

`pace::run(greet)` 创建事件循环，在当前线程上启动 `greet(io)`，拿到结果后
请求停止，并处理后台任务的取消。因此这里不需要手写 `spawn` 和 `stop`。
入口返回 `task<void>` 时，`run` 也返回 `void`。

## 第二步：给任务传入数据

将函数改为 `greet(pace::io_context& io, std::string name)`，
最后返回 `"Hello, " + name`。字符串按值传入，等待期间仍由协程自己持有。
现在在 `main` 中用工厂函数补上额外参数：

```cpp
const auto message = pace::run([](pace::io_context& io) {
    return greet(io, "reader");
});
```

这里的 lambda 是普通函数，返回一个尚未执行的 task。
如果想直接写协程 lambda，也可以交给 `run`：

```cpp
const auto answer = pace::run([value = 42](pace::io_context& io) -> pace::task<int> {
    co_await io.yield();
    co_return value;
});
```

`run` 会持有整个闭包直到运行及取消结束，所以等待后仍能读取 `value`。
这不代表任意临时协程 lambda 都安全：不要写
`io.spawn([capture]() -> task<void> { ... }())`，临时闭包可能先于协程销毁。
引用捕获仍然只是借用；被引用的对象必须活得足够久。

## 第三步：组合两个异步操作

在另一个协程中，直接写：

```cpp
const auto message = co_await greet(io, "reader");
```

父任务会在子任务产生结果之后继续。如果先把 task 保存为 `pending`，
则使用 `co_await std::move(pending)` 消费它。task 是惰性的、不可复制的，
只能被消费一次。调用函数得到 task 并不等于已经在后台运行它。

子任务抛出的异常会在 `co_await` 处重新抛出；能恢复就在那里捕获，
否则让异常到达 `main`。试着在定时器后抛出异常，确认程序报告错误并退出。
`io.yield()` 和零时长定时器只是让出一轮调度，不会把计算搬到另一个线程。

## 第四步：区分“入口结束”和“服务停止”

`run` 适合有明确结束点的入口。它返回的数据应当独立拥有内容，
例如数字、字符串；不要返回绑定到临时事件循环的 socket，
也不要返回指向入口局部变量或闭包内容的 view。

入口可以 `io.spawn` 后台任务，但入口完成意味着取消这些任务，
而不是等待它们自然完成。后台任务应按值持有数据，或者借用 `run` 外、
直到 `run` 返回都仍存活的对象。入口局部变量可能先于后台取消处理被销毁。

需要自己安排多个根任务、停止接收和排空连接时，使用显式形式：

```cpp
pace::io_context io;
io.spawn(service(io)); // service 返回 task<void>
io.run();             // 需要由某个控制路径显式调用 io.stop()
```

`io.run()` 不会因为最后一个任务结束就自动退出；`pace::run(factory)` 会。
两者都不能在同一线程正在运行的 PACE 事件循环内嵌套调用。
入口被取消时，`pace::run` 抛出 `operation_canceled`，不会悄悄给出空结果。

最后运行 `xmake run tutorial_tasks`，对照[扩展练习源码](../../examples/tutorial_tasks.cpp)
检查子任务异常、惰性启动和上下文归属。下一章用相同写法连接 TCP。

[完整入口契约](../reference/run.md) · [task 参考](../reference/task.md)
