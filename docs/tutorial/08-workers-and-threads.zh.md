# 5. 明确工作在哪个线程上执行

[English](05-execution.md) · [上一章：HTTP](04-http.zh-CN.md) · [下一章：关闭](06-shutdown.zh-CN.md)

前面的 `pace::run` 和手动 `io.run()` 都在调用线程执行。本章把两个任务分配到
两个独立 reactor，理解“异步等待”和“多线程并行”的区别。依赖仍是 `pace_runtime`。

## 第一步：给每个任务选一个 context

用下面的完整程序替换 `main.cpp`：

```cpp
#include <pace/runtime.hpp>
#include <atomic>
#include <chrono>
#include <iostream>

pace::task<void> work(pace::runtime& pool, std::size_t index, std::atomic<unsigned>& remaining) {
    auto& io = pool.context(index);
    co_await io.sleep_for(std::chrono::milliseconds(5));
    if (remaining.fetch_sub(1) == 1) pool.stop();
}

int main() {
    try {
        pace::runtime pool(2);
        std::atomic<unsigned> remaining{2};
        pool.context(0).spawn(work(pool, 0, remaining));
        pool.context(1).spawn(work(pool, 1, remaining));
        pool.run_on_current_thread();
        std::cout << "Both workers finished\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

构建并运行 `my_application`，预期输出 `Both workers finished`。
`pool(2)` 的 2 是总 context 数，`run_on_current_thread()` 用调用线程运行第零个，
再开一个后台线程运行另一个。`remaining` 会被两个线程访问，因此使用 atomic。
最后完成的任务停止整个 runtime，run 返回前已完成 join，外层对象可以安全销毁。

## 第二步：改变线程布局，不改变任务

把调用换成 `pool.run()`，现在两个 context 都由后台线程执行，调用线程等待它们结束。
两种调用都会阻塞，都不是“启动后立刻返回”的后台服务接口。
`runtime(1).run_on_current_thread()` 不创建额外线程；省略数量则按硬件并发数选择，
不等于你的业务一定在这个数量下最快。runtime 是单次运行对象。

任务显式分配到 `context(index)`，之后不会自动迁移或被其他 worker 偷走。
同一个 context 不能同时由多个线程运行，也不能在正在运行的 PACE 循环里嵌套 run。

## 第三步：从其他线程投递工作

已有应用线程时，可以在那个线程调用 `io.run()`，不必使用 runtime。
控制线程可以 `io.spawn(owned_task)`、`io.post(callback)` 或 `io.stop()`；
其中 post 是普通同步回调，不是协程：

```cpp
io.post([&io] { io.stop(); });
```

引用的 context 必须活到回调执行，关闭期间投递可能被拒绝。
外部线程必须在销毁 context 前 join，异常路径也一样。
[英文完整教程](05-execution.md#run-without-a-runtime)给出了保留异常并 join 的代码。

## 第四步：别把阻塞调用当成异步操作

在协程里同步读文件、调用阻塞数据库驱动或执行长计算，仍然会占住所在 worker。
`yield()` 只是给同一 context 的其他任务让一次机会，不会把计算移走。
PACE 当前没有阻塞工作 offload API；需要应用自己的工作线程，并把结果按值 post 回
原 context。关闭时先保证这些线程不会再访问已经销毁的 context。

HTTP/2、HTTP/3 的并发 handler 也遵循同样规则：等待可以重叠，单个 context 的 CPU
代码并不并行。跨 worker 共享的路由闭包和可变状态仍然需要同步。

## 第五步：核对线程归属

在 PACE 根目录运行 `xmake run tutorial_workers`，再运行
`xmake run tutorial_workers background`。这个[配套检查](../../examples/tutorial_workers.cpp)
不仅看最终输出，还验证每个任务确实留在指定 context，以及 caller 模式下第零个
context 是否就在 main 的线程上。

需要多个 worker 监听同一端口时，参考各 worker 独立 listener 的 reuse-port 示例；
该策略并非所有平台都支持，不要把活动 socket 随意移动到另一个 context。
[执行契约](../reference/runtime.md) · [平台说明](../reference/platform.md)
