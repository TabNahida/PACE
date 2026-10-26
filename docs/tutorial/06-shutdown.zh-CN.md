# 6. 从请求停止到安全销毁

[English](06-shutdown.md) · [上一章：执行方式](05-execution.zh-CN.md) · [下一章：UDP](07-udp.zh-CN.md)

这一章先亲眼验证取消，再把它用于服务器。依赖 `pace_runtime`，
将下面完整程序保存到 `main.cpp` 并构建运行：

```cpp
#include <pace/io_context.hpp>
#include <chrono>
#include <iostream>

pace::task<void> waiting(pace::io_context& io, bool& canceled) {
    try { co_await io.sleep_for(std::chrono::hours(1)); }
    catch (const std::system_error& error) {
        if (error.code() != std::errc::operation_canceled) throw;
        canceled = true;
    }
}

int main() {
    try {
        pace::io_context io;
        bool canceled = false;
        auto stop = io.call_after(std::chrono::milliseconds(10), [&io] { io.stop(); });
        io.spawn(waiting(io, canceled));
        io.run();
        if (!canceled) throw std::runtime_error("cancellation was not observed");
        std::cout << "Canceled and drained\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

应很快输出 `Canceled and drained`，而不是等待一小时。
10ms 定时器先请求停止，等待中的任务恢复并观察到取消，最后 `io.run()` 才返回。
定时器 handle 必须保存在变量里，否则临时对象销毁会立即取消回调。
`canceled` 在任务外面，一直活到 run 返回，所以取消时仍能安全写入。

## 第一步：认识取消的错误路径

取消通过 `std::system_error` 报告，错误码是 `operation_canceled`。
处理它时释放同步资源并返回；其他错误继续抛出。stop 后的新 I/O 和定时等待也会取消，
所以需要异步刷新的数据应当在 stop 前处理。

`stop()` 只是请求，不会强制中断线程。一个不暂停、不返回的循环，或阻塞调用，
仍然可能阻止关闭。`pace::run` 在入口完成时自动请求停止；它不是等待所有后台任务
自然完成的 join-all。显式 `io.run()` 则由应用决定何时停止。

## 第二步：把“停止接收”和“停止所有工作”分开

希望尽量完成已接收的 HTTP 请求时，先在所属 context 上关闭 listener，
再等已有连接完成，达到业务宽限期后才 stop。最后等待 run 返回或 join，
然后销毁网络对象与 context。

关闭 listener 只取消 accept，不会关闭已接受的 socket。
自己写 accept 循环时，应捕获这个预期取消并退出；否则 stop 前的未捕获根任务异常
会停止整个循环。`http::serve` 已有这条处理路径。

当前库没有公开的连接计数/join handle。需要精确 drain 时，写自己的 accept 包装，
给每个连接用 RAII 维护活动计数，配合有界宽限期。
[完整 drain 示例](06-shutdown.md#stop-accepting-before-stopping-the-reactor)
解释了计数和监听对象必须保持多久；不要把立即 stop 宣称为优雅排空。

## 第三步：等待成员协程退栈后再销毁对象

`socket.close()` 取消它注册的等待并释放底层句柄，但没有等待成员协程完成。
成员协程仍可能通过 `this` 访问 socket 对象，因此不能先销毁或移动 socket，
再指望取消替你修复生命周期。缓冲区、listener、TLS 配置和回调引用同样需要活得足够久。

`co_await socket.close_gracefully()` 是另一种操作：在没有其他并行 socket 操作时，
半关闭发送并有界 drain，再关闭句柄。默认有 100ms/64KiB 上限；到达上限是正常结果，
不是对端业务处理成功的证明。应在 stop 之前调用。

## 第四步：把外部所有者也收好

普通控制线程可以调用 `stop()`。信号处理器则只设置安全的标志，让正常任务读取后
请求停止；不要直接从信号处理器调用任意 reactor/socket API。

`runtime.run()` 和 `run_on_current_thread()` 会等待自己的 worker；
如果它们又运行在应用创建的外层线程上，应用仍需 join 那个外层线程。
任何还会 post 结果的业务线程也必须先停止/join，不能向已销毁的 context 投递。

运行 `xmake run tutorial_shutdown`，查看[配套练习](../../examples/tutorial_shutdown.cpp)
怎样额外验证 RAII 清理已在 run 返回前完成。
