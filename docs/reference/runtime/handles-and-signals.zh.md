<!-- source-hash: 876169915b2f -->
# 句柄与信号

[English](handles-and-signals.md) · [运行时](README.zh.md)

## 概述

在非 socket 描述符（管道、终端、TUN 设备）上等待就绪，在 Windows 内核对象句柄上等待，以及把进程信号（`SIGINT`、
`SIGTERM`、`SIGHUP`、`SIGUSR1`、`SIGUSR2` 及其 Windows 对应事件）变成协程可以 `co_await` 的值。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/runtime/io_context.hpp>`（也可用 `<pace/runtime.hpp>`） | `pace_runtime` | 始终构建 |
| `<pace/runtime/signal.hpp>`（也可用 `<pace/runtime.hpp>`） | `pace_runtime` | 始终构建 |

`io_context::wait_readable`、`wait_writable` 和 `wait_ready`（见 [`io_context`](io-context.zh.md#就绪等待)）接受后端
能够轮询的任何描述符，不限于 socket。Windows 没有这类描述符；在那里改用 `io_context::wait_handle` 等待事件、进程或其他
内核对象。`signal_set` 与 `wait_for_signal` 把信号变成可等待的值，服务只需两行代码就能优雅关闭。

## 概要

收到 Ctrl-C 或 `SIGTERM` 时优雅关闭：提供服务的任务运行在一个任务组里，信号到来后取消这个组。

```cpp
#include <pace/runtime.hpp>

#include <chrono>
#include <iostream>

pace::task<void> serve_forever()
{
    for (;;)
    {
        co_await pace::sleep_for(std::chrono::seconds(1)); // 代替一个 accept 循环
    }
}

int main()
{
    pace::run(
        []() -> pace::task<void>
        {
            co_await pace::with_task_group(
                [](pace::task_group& group) -> pace::task<void>
                {
                    (void)group.spawn(serve_forever());
                    co_await pace::wait_for_signal({pace::process_signal::interrupt, pace::process_signal::terminate});
                    group.cancel(); // 服务任务收到 operation_canceled 并展开，任务组随后汇合它们
                }
            );
            std::cout << "stopped\n";
        }
    );
}
```

长期保留的订阅：每次收到 `SIGHUP` 就重新加载配置，然后继续运行：

```cpp
pace::task<void> reload_on_hangup(pace::io_context& io)
{
    pace::signal_set signals(io, {pace::process_signal::hangup});
    for (;;)
    {
        co_await signals.wait(); // 重新加载期间到达的 SIGHUP 由下一次等待报告
        reload_configuration();
    }
}
```

## 类型

### `process_signal`

```cpp
enum class process_signal
{
    hangup,
    interrupt,
    terminate,
    user1,
    user2
};
```

| 值 | POSIX | Windows |
|:--|:--|:--|
| `hangup` | `SIGHUP` | 只能通过 `deliver_signal` |
| `interrupt` | `SIGINT` | `CTRL_C_EVENT`、`CTRL_BREAK_EVENT` |
| `terminate` | `SIGTERM` | `CTRL_CLOSE_EVENT`、`CTRL_LOGOFF_EVENT`、`CTRL_SHUTDOWN_EVENT`；服务的停止请求通过 `deliver_signal` |
| `user1` | `SIGUSR1` | 只能通过 `deliver_signal` |
| `user2` | `SIGUSR2` | 只能通过 `deliver_signal` |

### `signal_set`

```cpp
class signal_set
{
public:
    struct awaiter;

    signal_set(io_context& io, std::initializer_list<process_signal> signals);
    ~signal_set();
    signal_set(const signal_set&) = delete;
    signal_set& operator=(const signal_set&) = delete;

    [[nodiscard]] awaiter wait() noexcept;
    [[nodiscard]] bool contains(process_signal value) const noexcept;
    [[nodiscard]] io_context& context() const noexcept;
};
```

一个上下文对若干信号的订阅。不可复制，也不可移动。

| 成员 | 行为 |
|:--|:--|
| `signal_set(io, signals)` | 订阅 `signals`（重复项被忽略）。从此刻起，这些信号的每一次递送都为本集合计数，无论当时是否有等待挂起。列表为空或值未知时抛出 `std::invalid_argument`；无法安装信号处置，或所有通知槽都已占用时抛出 `std::system_error`。 |
| `~signal_set()` | 取消订阅。若它是进程中某个信号的最后一个订阅，则恢复该信号先前的处置。 |
| `wait()` | `co_await` 挂起，直到其中某个信号被递送，并返回该信号。已经计数的递送使它立即完成。多个待报告的递送按枚举顺序（`hangup` 最先）每次等待报告一个。 |
| `contains(value)` | `value` 是否在订阅之列。 |
| `context()` | 传给构造函数的上下文。 |

**所有权模型。** 信号处置是进程级的，按信号做引用计数：

- 进程中第一个订阅某信号的 `signal_set` 安装 PACE 的处理函数（POSIX 上用带 `SA_RESTART` 的 `sigaction`；Windows
  上为 `interrupt` 和 `terminate` 注册控制台控制处理函数），并保存先前的处置；最后一个被销毁时将其恢复。PACE 持有某
  信号期间，被替换的处理函数不会被调用，操作系统的默认动作（`SIGINT`、`SIGTERM` 的默认动作是终止进程）也不会执行。
- 递送是广播的。订阅了某信号的每个 `signal_set`，无论在哪个上下文、哪个线程，都能看到它构造之后发生的每一次递送；同一
  上下文中的多个集合也各自看到。
- 两次等待之间的递送不会丢失：每个集合记住自己已经报告了多少次递送。集合创建之前的递送不会报告给它。
- 在 `signal_set` 订阅某信号期间自行修改其处置（`sigaction`、`signal`、`SetConsoleCtrlHandler`）不受支持；之后的恢复
  会覆盖你的修改。

**Windows。** `terminate` 类事件（关闭控制台窗口、注销、关机）的控制台处理函数在递送后会阻塞所在线程，因为处理函数一
返回 Windows 就会结束进程；程序在系统给出的宽限期内完成关闭并从 `main` 返回。服务收不到控制台事件：请在服务控制处理
函数中收到 `SERVICE_CONTROL_STOP` 和 `SERVICE_CONTROL_SHUTDOWN` 时调用 `deliver_signal(process_signal::terminate)`。

### `signal_set::awaiter`

```cpp
struct awaiter
{
    signal_set& set;
    std::error_code error;
    process_signal value{};
    std::coroutine_handle<> continuation;
    cancellation_token token{};
    io_context::cancellation_handle registration{};
    awaiter* previous = nullptr;
    awaiter* next = nullptr;
    bool linked = false;

    explicit awaiter(signal_set& owner) noexcept;
    awaiter(const awaiter&) = delete;
    awaiter& operator=(const awaiter&) = delete;
    ~awaiter();
    bool await_ready();
    template <class P>
    bool await_suspend(std::coroutine_handle<P> handle);
    process_signal await_resume();
};
```

`wait()` 的结果：在上下文的线程上立即 `co_await` 一次，挂起期间不要移动或修改它。`await_ready` 消费一次已经计数的递送。
`await_suspend` 复制等待方任务的取消令牌，为它注册 `on_cancel` 回调，并把等待登记到上下文。`await_resume` 返回信号或
抛出异常。若协程在挂起时被销毁，析构函数会把这次等待摘除。

### `io_context::handle_awaiter`（Windows）

```cpp
#ifdef _WIN32
struct handle_awaiter
{
    io_context& context;
    HANDLE object;
    std::error_code error;
    std::coroutine_handle<> continuation;
    cancellation_token token{};
    cancellation_handle registration{};
    void* wait = nullptr;
    std::uint64_t id = 0;
    bool await_ready() noexcept;
    template <class P>
    bool await_suspend(std::coroutine_handle<P> waiting);
    void await_resume();
};
#endif
```

`wait_handle` 的结果，规则与 `io_awaiter` 相同。协程挂起期间，`wait` 是线程池等待对象，`id` 是它在上下文中的键。

## 函数

### 非 socket 描述符上的就绪等待

```cpp
io_awaiter wait_readable(native_socket_type fd) noexcept;
io_awaiter wait_writable(native_socket_type fd) noexcept;
io_awaiter wait_ready(native_socket_type fd) noexcept;
```

在 POSIX 上 `native_socket_type` 就是 `int`，这些等待在每个后端上都适用于驱动支持轮询的任何描述符。反应器不对描述符做
任何 socket 调用（不读 `SO_ERROR`，不调 `recv`）：它只报告就绪，非阻塞的 `read` 或 `write` 由你随后执行。

| 描述符 | epoll | io_uring | kqueue |
|:--|:--|:--|:--|
| 管道、FIFO | 支持 | 支持 | 支持 |
| 终端、伪终端 | 支持 | 支持 | 支持 |
| `eventfd`、`timerfd`、`signalfd`、`pidfd`、`inotify`（Linux） | 支持 | 支持 | — |
| Linux TUN `/dev/net/tun`、每个 `IFF_MULTI_QUEUE` 队列、Android `VpnService` 描述符 | 支持 | 支持 | — |
| macOS 与 iOS 的 `utun`（一个 `PF_SYSTEM` socket） | — | — | 支持 |
| FreeBSD `/dev/tun*` | — | — | 支持 |
| 普通文件 | 拒绝（`EPERM`） | 总是就绪 | 到达文件末尾之前就绪 |
| 不支持轮询的设备（`/dev/null`） | 拒绝（`EPERM`） | 总是就绪 | 拒绝（`EINVAL`） |

被拒绝的描述符会让 `co_await` 抛出带内核错误码的 `std::system_error`。请自行把描述符设为非阻塞（`O_NONBLOCK`）：就绪等待
不会修改它，唤醒之后的阻塞 `read` 仍可能阻塞。TUN 描述符每次 `read` 返回一个数据包；再次等待之前要一直读到 `EAGAIN`。
关闭一个等待过的描述符之前，调用 `io.release_socket(fd)`（名字是历史原因：它接受任何描述符）。
[TUN 指南](../../guides/tun-devices.zh.md)给出了完整的读写循环。

在 Windows 上，就绪等待只接受 Winsock socket；其他对象请用 `wait_handle`。

### `io_context::wait_handle`（Windows）

```cpp
#ifdef _WIN32
handle_awaiter wait_handle(HANDLE object) noexcept;
#endif
```

`co_await io.wait_handle(object)` 挂起，直到一个可等待的内核对象变为有信号：事件（例如 Wintun 的
`WintunGetReadWaitEvent`）、信号量、进程或线程、可等待计时器、变更通知。与 `WaitForSingleObject` 一样，等待会消费自动
重置事件或信号量的信号。不支持互斥体：所有权会落在线程池线程上。同一个句柄上可以有任意多个等待，彼此独立。句柄必须保持
打开，直到等待结束。

每个等待注册一个线程池等待（`CreateThreadpoolWait`）；其回调把这次等待的键投递给上下文，从而唤醒轮询器：IOCP 后端上
是一个完成包，WSAPoll 后端上是回环唤醒数据报（线程池的等待线程就是这个后端的辅助线程）。取消和 `stop()` 会注销线程池
等待，并在协程恢复之前等待正在运行的回调结束，因此没有回调会比这次操作活得更久。

POSIX 没有 `wait_handle`。对应的做法是在描述符上等待就绪：事件用 `eventfd`，进程用 `pidfd`（Linux），其他情况用管道。

### `wait_for_signal`

```cpp
task<process_signal> wait_for_signal(io_context& io, std::initializer_list<process_signal> signals);
task<process_signal> wait_for_signal(std::initializer_list<process_signal> signals);
```

在**调用**时订阅 `signals`，并返回一个任务：它等待第一次递送并返回该信号。任务持有这份订阅，所以调用之后、等待任务之前
发出的信号不会丢失；销毁任务即取消订阅。第二种形式使用 `this_context()`，当前线程没有运行上下文时抛出
`std::logic_error`。需要反复接收信号时，请改为保留一个 `signal_set`。

### `deliver_signal`

```cpp
void deliver_signal(process_signal value) noexcept;
```

把 `value` 递送给每个订阅了它的 `signal_set`，效果如同操作系统发出了该信号，但不会执行任何默认动作或被替换的处理函数。
它是线程安全的，在 POSIX 上也是异步信号安全的。可以在 Windows 服务控制处理函数中使用，也可以在程序内部触发重新加载或
关闭。没有订阅者时什么也不做。

```cpp
DWORD WINAPI service_control(DWORD control, DWORD, LPVOID, LPVOID)
{
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN)
    {
        report_stop_pending();
        pace::deliver_signal(pace::process_signal::terminate);
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}
```

### `signal_name`

```cpp
const char* signal_name(process_signal value) noexcept;
```

在所有平台上返回 `"SIGHUP"`、`"SIGINT"`、`"SIGTERM"`、`"SIGUSR1"` 或 `"SIGUSR2"`；其他值返回 `"unknown"`。

## 错误

| 错误 | 何时 |
|:--|:--|
| `std::system_error(operation_canceled)` | 上下文停止时挂起中的就绪、句柄或信号等待；`cancel(fd)` 之后的就绪等待 |
| 带令牌错误码的 `std::system_error` | 所在任务的令牌被取消的等待（`operation_canceled`，截止时间到期则为 `timed_out`） |
| `std::system_error`（内核错误码） | 在后端拒绝的描述符上等待（`EPERM`、`EINVAL`）；`CreateThreadpoolWait` 失败；创建集合或首次等待时 `sigaction`、`SetConsoleCtrlHandler`、`pipe` 或 `CreateEventW` 失败 |
| `std::system_error(bad_file_descriptor)` | 对空句柄或 `INVALID_HANDLE_VALUE` 调用 `wait_handle` |
| `std::system_error(resource_unavailable_try_again)` | 同时等待信号的上下文超过 256 个 |
| `std::invalid_argument` | `signal_set` 的列表为空，或包含 `process_signal` 之外的值 |
| `std::logic_error` | 同一个 `signal_set` 上的第二个挂起等待；在没有运行其上下文的线程上等待；不带 `io` 的 `wait_for_signal` 在没有运行中上下文时调用 |

## 线程与并发

- 就绪等待和句柄等待遵循 [`io_context`](io-context.zh.md#线程与并发) 的规则：它们运行在上下文的线程上，一个描述符每个
  方向最多一个挂起等待。`wait_handle` 没有这个限制。
- `signal_set` 可以在任何线程上创建和销毁，也可以在 `run()` 之前。`wait()` 运行在运行其上下文的线程上，每个集合一个挂起
  等待；需要多个并发等待者时请使用多个集合。
- PACE 的 POSIX 信号处理函数只递增无锁计数器，并向每个等待信号的上下文的管道写一个字节；除此之外不调用任何函数，并保留
  `errno`。每个这样的上下文有一个内部泵任务等待自己的管道（普通的就绪等待，因此包括 io_uring 在内的所有后端都无需改动），
  并恢复信号已到达的那些等待。在 Windows 上，通知对象是一个用 `wait_handle` 等待的自动重置事件。
- 有意不使用 `signalfd`：它要求信号在进程的每个线程中都被阻塞，而且一次读取会替所有上下文消费掉信号。kqueue 上的
  `EVFILT_SIGNAL` 可以工作，但无论如何都需要一个处理函数来阻止默认动作，所以所有 POSIX 系统共用一条路径。
- `deliver_signal` 可以在任何线程上调用，也可以在 POSIX 信号处理函数中调用。

## 生命周期与所有权

- 就绪等待借用描述符，`wait_handle` 借用句柄；两者都必须保持打开直到等待结束。关闭一个等待过的描述符之前调用
  `release_socket(fd)`。
- `signal_set` 拥有自己的订阅。只在它上面没有挂起等待时销毁它，除非等待它的协程本身正在被销毁。它可以比上下文活得更久，
  但之后不能再在它上面等待。
- 上下文从第一次信号等待起保留自己的信号通知对象（一个管道或一个事件），直到上下文被销毁。
- `wait_for_signal` 返回的任务拥有其 `signal_set`。

## 限制与默认值

| 项 | 值 |
|:--|:--|
| 信号 | `SIGHUP`、`SIGINT`、`SIGTERM`、`SIGUSR1`、`SIGUSR2` |
| POSIX 处理函数标志 | `SA_RESTART`，空掩码 |
| 每个 `signal_set` 的挂起等待 | 1 |
| 同时等待信号的上下文 | 256 |
| 多个待报告递送的顺序 | `process_signal` 的枚举顺序 |
| 每个信号的递送计数 | 计数器 2³² 后回绕；差值保持精确 |
| 每个等待中上下文的通知对象 | 一个非阻塞管道（POSIX）或自动重置事件（Windows） |
| 每个句柄上的 `wait_handle` 等待 | 不限；每个对应一个线程池等待 |

同一信号在挂起期间到达的多次递送可能被操作系统合并；PACE 报告其处理函数看到的每一次递送。

## 参见

- [`io_context`](io-context.zh.md)：就绪等待、`cancel`、`release_socket`
- [结构化并发](structured-concurrency.zh.md)：任务组与取消令牌
- [指南：TUN 设备](../../guides/tun-devices.zh.md)
- [教程：有序关闭](../../tutorial/09-graceful-shutdown.zh.md)
- [I/O 后端](io-backends.zh.md) · [平台支持](../platform-support.zh.md)
- Microsoft：[线程池等待](https://learn.microsoft.com/en-us/windows/win32/api/threadpoolapiset/nf-threadpoolapiset-createthreadpoolwait)、
  [控制台控制处理函数](https://learn.microsoft.com/en-us/windows/console/handlerroutine)；
  [Wintun](https://www.wintun.net/)
