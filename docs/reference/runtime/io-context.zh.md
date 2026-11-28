<!-- source-hash: 1f464086b780 -->
# `pace::io_context`

[English](io-context.md) · [运行时](README.zh.md)

## 概述

一个 `io_context` 就是由一个线程运行的一个反应器：它拥有原生就绪轮询器、就绪协程队列、投递回调队列，
以及它所运行的 task 的定时器。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/runtime/io_context.hpp>`（也可用 `<pace/runtime.hpp>`） | `pace_runtime` | 始终构建 |
| `<pace/runtime/platform.hpp>`（也可用 `<pace/runtime.hpp>`） | `pace_runtime` | 始终构建 |

大多数程序不需要直接创建它：[`pace::run`](run.zh.md) 为一个入口 task 创建新的 context，
[`pace::runtime`](runtime.zh.md) 为每个工作线程持有一个。需要自己控制运行线程和生命周期时，再自行创建
`io_context`。socket、listener、定时器和等待都只属于一个 context。

投递、定时器和取消回调可以移动捕获独占资源，例如 `std::unique_ptr`。队列持有回调直至执行或取消。

## 概要

```cpp
#include <pace/runtime.hpp>

#include <chrono>

using namespace std::chrono_literals;

pace::task<void> tick(pace::io_context& io)
{
    co_await io.sleep_for(10ms);
    io.stop(); // 队列排空后 run() 返回
}

int main()
{
    pace::io_context io;
    auto guard = io.call_after(1s, [&io] { io.stop(); }); // 持有句柄才能保持定时器有效
    io.spawn(tick(io));
    io.run();
}
```

## 类型

### `io_context`

```cpp
class io_context
{
public:
    explicit io_context(io_backend selected = io_backend::automatic);
    [[nodiscard]] io_backend backend() const noexcept;
    ~io_context();
    io_context(const io_context&) = delete;
    io_context& operator=(const io_context&) = delete;

    std::shared_ptr<void> local_state(
        std::shared_ptr<const void> key,
        const std::function<std::shared_ptr<void>()>& create
    );
    void* find_local_state(const void* key);
    void spawn(task<void> work);
    void post(pace::unique_function<void()> work);
    void run();
    void stop() noexcept;
    bool stopped() const noexcept;
    static io_context* current() noexcept;
    void release_socket(native_socket_type fd) noexcept;
    void cancel(native_socket_type fd) noexcept;

    class timer_handle;
    [[nodiscard]] timer_handle call_at(std::chrono::steady_clock::time_point deadline, pace::unique_function<void()> callback);
    template <class Rep, class Period>
    [[nodiscard]] timer_handle call_after(std::chrono::duration<Rep, Period> duration, pace::unique_function<void()> callback);

    class cancellation_handle;
    [[nodiscard]] cancellation_handle on_cancel(cancellation_token, pace::unique_function<void(std::error_code)>);

    struct io_awaiter;
    io_awaiter wait_readable(native_socket_type fd) noexcept;
    io_awaiter wait_writable(native_socket_type fd) noexcept;
    io_awaiter wait_ready(native_socket_type fd) noexcept;
#ifdef _WIN32
    struct handle_awaiter;
    handle_awaiter wait_handle(HANDLE object) noexcept;
#endif

    struct timer_awaiter;
    template <class Rep, class Period>
    timer_awaiter sleep_for(std::chrono::duration<Rep, Period> duration) noexcept;

    struct yield_awaiter;
    yield_awaiter yield() noexcept;
};
```

context 既不能复制，也不能移动。

#### 构造与析构

| 成员 | 行为 |
|:--|:--|
| `io_context(selected)` | 为 `selected` 创建轮询器；`automatic` 会先查看 `PACE_IO_BACKEND`（见 [I/O 后端](io-backends.zh.md)）。无法创建原生轮询器或后端不可用（`std::errc::not_supported`）时抛出 `std::system_error`，`PACE_IO_BACKEND` 取值未知时抛出 `std::invalid_argument`。 |
| `backend()` | 实际使用的后端，不会是 `automatic`。 |
| `~io_context()` | 请求停止，取消已注册的等待，然后依次销毁排队的 task 和回调、仍持有的根任务协程帧以及 `local_state` 服务。不会运行尚未开始的协程体。它不会 join 线程：`run()` 必须已经返回。 |

#### 提交与运行

| 成员 | 行为 |
|:--|:--|
| `spawn(work)` | 接管一个根任务并调度它。协程体不会在 `spawn` 内部内联运行。空 task 被忽略。线程安全，可在 `run()` 之前或期间调用。context 停止后抛出 `std::system_error(operation_canceled)`。 |
| `post(work)` | 把回调排入队列，在本 context 上运行，从不内联执行。线程安全。函数为空时抛出 `std::invalid_argument`，停止后抛出 `std::system_error(operation_canceled)`。 |
| `run()` | 在调用线程上运行 context，直到请求了 `stop()` 且排队的工作已排空，然后重新抛出记录的第一个错误。空闲的 context 会一直等待：既没有 work guard，也没有“没有工作就返回”。若本线程已在运行某个 context（嵌套运行）、另一线程正在运行本 context，或本 context 已经运行过，则抛出 `std::logic_error`。 |
| `stop()` | 请求永久停止。线程安全、幂等、不等待。它唤醒轮询器，但不关闭 socket。 |
| `stopped()` | 是否已请求停止。线程安全。为 `true` 并不表示 `run()` 已经返回。 |
| `current()` | 调用线程上正在由 `run()` 执行的 context，或 `nullptr`。 |

请求停止后，context 会取消所有已注册的就绪等待和定时器（它们以 `operation_canceled` 恢复），派发其 `on_cancel`
注册，丢弃尚未触发的回调定时器，并继续运行就绪协程和投递回调，直到所有队列为空。排空期间恢复的协程会观察到停止，
应当尽快结束；此时 `spawn` 和 `post` 已经拒绝新工作。context 不能重新启动。

#### context 本地服务

| 成员 | 行为 |
|:--|:--|
| `local_state(key, create)` | 返回以地址 `key.get()` 存放的服务，首次调用时用 `create` 创建。context 持有 `key` 和值直到自身销毁；值可以拥有 socket。key 为空、`create` 为空或其结果为空时抛出 `std::invalid_argument`，停止后抛出 `std::system_error(operation_canceled)`。 |
| `find_local_state(key)` | 返回 `key` 对应的值或 `nullptr`，不改变引用计数。指针在 context 销毁前一直有效。停止后抛出 `std::system_error(operation_canceled)`。 |

两者都必须在运行本 context 的线程上调用（否则抛出 `std::logic_error`）。它们用于有界的、按 context 划分的服务，
例如连接池，而不是每个请求的数据。

#### 原始描述符

| 成员 | 行为 |
|:--|:--|
| `cancel(fd)` | 注销该描述符上挂起的等待，并以 `operation_canceled` 恢复它们。未知描述符不做任何事。它不关闭描述符，也不停止 context，之后仍可再次等待该描述符。 |
| `release_socket(fd)` | 先 `cancel(fd)`，再丢弃后端对它的注册。自行关闭原始 socket 前立即调用它，这样复用同一编号的新描述符永远不会匹配旧注册。 |

两者都是 `noexcept`，必须在 context 的线程上运行，或在没有线程运行它时（`run()` 之前或返回之后）调用。
在其他线程上，用 `post` 投递一个调用它们的回调。PACE 自己的 socket 类型会自动完成这些操作。

### `io_context::timer_handle`

```cpp
class timer_handle
{
public:
    timer_handle() noexcept = default;
    ~timer_handle();
    timer_handle(timer_handle&&) noexcept;
    timer_handle& operator=(timer_handle&&) noexcept;
    timer_handle(const timer_handle&) = delete;
    timer_handle& operator=(const timer_handle&) = delete;
    void cancel() noexcept;
};
```

`call_at` 或 `call_after` 创建的一个回调定时器的所有者。句柄存在期间定时器保持有效。`cancel()`、析构，
以及对已持有定时器的句柄进行移动赋值，都会立即撤销定时器并销毁回调（连同其捕获）。句柄可以比 context 活得更久，
此时取消它不做任何事。

### `io_context::cancellation_handle`

```cpp
class cancellation_handle
{
public:
    cancellation_handle() noexcept = default;
    ~cancellation_handle();
    cancellation_handle(cancellation_handle&&) noexcept;
    cancellation_handle& operator=(cancellation_handle&&) noexcept;
    cancellation_handle(const cancellation_handle&) = delete;
    cancellation_handle& operator=(const cancellation_handle&) = delete;
    void cancel() noexcept;
};
```

一个 `on_cancel` 注册的所有者。`cancel()`、析构，以及对有效句柄进行移动赋值，都会注销回调；届时尚未运行的回调永远不会运行。
句柄可以比 context 活得更久。

### `io_context::io_awaiter`、`timer_awaiter`、`yield_awaiter`

```cpp
struct io_awaiter
{
    io_context& context;
    native_socket_type fd;
    bool writable;
    std::error_code error;
    std::coroutine_handle<> continuation;
    bool either = false;
    cancellation_token token{};
    cancellation_handle registration{};
    bool await_ready() noexcept;
    template <class P>
    bool await_suspend(std::coroutine_handle<P> handle);
    void await_resume();
};

struct timer_awaiter
{
    io_context& context;
    std::chrono::steady_clock::time_point deadline;
    std::error_code error;
    std::coroutine_handle<> continuation;
    cancellation_token token{};
    cancellation_handle registration{};
    std::size_t heap_index = static_cast<std::size_t>(-1);
    bool await_ready() noexcept;
    template <class P>
    bool await_suspend(std::coroutine_handle<P> handle);
    void await_resume();
};

struct yield_awaiter
{
    io_context& context;
    cancellation_token token{};
    bool await_ready() const noexcept;
    template <class P>
    void await_suspend(std::coroutine_handle<P> handle);
    void await_resume();
};
```

它们是 `wait_*`、`sleep_for` 和 `yield` 的返回值，字段就是一次挂起等待的状态：只通过这些函数创建，
在所属 context 上立即 `co_await` 一次，挂起期间不要移动或修改它们。

- `io_awaiter` 与 `timer_awaiter`：只有 context 已经停止时 `await_ready` 才为真（这次 await 随后以
  `operation_canceled` 失败）。`await_suspend` 复制正在 await 的 task 的令牌；令牌已被取消则立即失败，
  否则为它注册 `on_cancel` 回调，并向反应器注册等待。`await_resume` 抛出令牌的错误、保存的错误，
  或在 context 已停止时抛出 `operation_canceled`。
- `yield_awaiter`：从不就绪；`await_suspend` 把协程放到就绪队列末尾；若 task 的令牌已被取消或 context 已停止，
  `await_resume` 抛出异常。

`heap_index` 是定时器挂起期间在反应器截止时间堆中的位置。

### `scheduler_access`

```cpp
struct scheduler_access
{
    static void schedule(io_context& io, std::coroutine_handle<> handle);
};
```

供任务组使用的实现支持：把一个挂起的协程放入 `io` 的就绪队列。必须在运行 `io` 的线程上调用。
它不能用来从其他线程恢复协程；那种情况请用 `post`。

### 平台类型

```cpp
// <pace/runtime/platform.hpp>
using native_socket_type = int;            // std::uintptr_t on Windows
inline constexpr native_socket_type invalid_socket = static_cast<native_socket_type>(-1);
```

`native_socket_type` 是所有就绪等待和 PACE socket 使用的描述符类型：POSIX 文件描述符，或 Windows 上的
`SOCKET` 值。`invalid_socket` 表示“没有 socket”；等待它会以 `std::errc::bad_file_descriptor` 失败。

## 函数

### 就绪等待

```cpp
io_awaiter wait_readable(native_socket_type fd) noexcept;
io_awaiter wait_writable(native_socket_type fd) noexcept;
io_awaiter wait_ready(native_socket_type fd) noexcept;
```

`co_await io.wait_readable(fd)` 挂起到描述符可读（或报告错误、流结束）；`wait_writable` 对应可写；
`wait_ready` 挂起到任一方向可以推进，即使两个方向同时就绪也只恢复一次。它们不传输字节，也不返回就绪掩码：
等待结束后重试非阻塞操作。它们不拥有描述符，描述符必须保持打开、身份不变，直到等待结束。
PACE 的 TCP 和 UDP 类型会替你完成这个循环。

在 POSIX 上描述符不必是 socket：管道、终端、TUN 设备以及 Linux 的 `eventfd` 一族在每个后端上都可用。在 Windows 上
就绪等待只接受 Winsock socket，`wait_handle` 用来等待内核对象。这两者以及可等待的进程信号见
[句柄与信号](handles-and-signals.zh.md)。

### 定时器与让出

```cpp
template <class Rep, class Period>
timer_awaiter sleep_for(std::chrono::duration<Rep, Period> duration) noexcept;
yield_awaiter yield() noexcept;
```

`sleep_for` 在**调用**时而不是 await 时计算 `steady_clock` 截止时间，因此要立即 await。零和负时长已经到期，
但仍会挂起一轮，所以连续的零时长 sleep 不会让 context 看不到 `stop()`。超出时钟范围的时长饱和到其最大值。
定时器不会早于截止时间触发，但可能因为轮询精度、其他就绪工作或阻塞代码而延迟。

`yield()` 把协程移到就绪队列末尾：其他就绪协程、投递回调和 I/O 先获得执行机会。

### 回调定时器

```cpp
[[nodiscard]] timer_handle call_at(std::chrono::steady_clock::time_point deadline, pace::unique_function<void()> callback);
template <class Rep, class Period>
[[nodiscard]] timer_handle call_after(std::chrono::duration<Rep, Period> duration, pace::unique_function<void()> callback);
```

设置一个一次性回调，在 `deadline`（或 `duration` 之后，计算方式与 `sleep_for` 相同）于本 context 上运行。
它从不在 `call_at` 内部调用。截止时间相同的回调按注册顺序运行；到期的回调在运行前就被移除。
它抛出的异常按投递回调的方式处理。停止 context 会丢弃剩余回调而不调用它们。

可以在 context 的线程上注册，或在 `run()` 之前注册。回调为空时抛出 `std::invalid_argument`，context 停止后抛出
`std::system_error(operation_canceled)`。限定作用域的截止时间就是用回调定时器构建的：回调可以取消或关闭操作，
正常完成时只需丢弃句柄。

### 取消回调

```cpp
[[nodiscard]] cancellation_handle on_cancel(cancellation_token, pace::unique_function<void(std::error_code)>);
```

自定义 awaiter 的挂钩。`token` 被取消时，`callback` 以令牌的错误码（`operation_canceled` 或 `timed_out`）
**在本 context 上**运行，并应当恰好一次地完成或解除它所守护的等待。取消请求可以来自任何线程；
只有回调的执行被转移到 context 上。context 停止时，剩余的每个注册都会运行，错误码为令牌自身的错误码，
令牌没有错误时为 `operation_canceled`。

- 空令牌（`stop_possible()` 为假）返回空句柄，不注册任何东西。
- 否则必须在 context 的线程上调用（否则抛出 `std::logic_error`）；context 停止后抛出
  `std::system_error(operation_canceled)`，回调为空时抛出 `std::invalid_argument`，令牌已被取消时抛出令牌的错误。
- 回调只在收到取消请求时触发。令牌的截止时间仅仅到期并不会触发它：这需要[操作边界](structured-concurrency.zh.md)
  设置的定时器，在截止时间到达时请求超时。
- 回调抛出的异常遵循下文反应器的错误策略。

### 平台辅助函数

```cpp
// <pace/runtime/platform.hpp>
inline void initialize_sockets();
inline void initialize_winsock();
#ifdef _WIN32
inline SOCKET create_windows_socket(int family, int type, int protocol) noexcept;
#endif
```

在 Windows 上，`initialize_sockets()` 在每个进程中启动一次 Winsock 2.2（线程安全；退出时运行 `WSACleanup`），
`WSAStartup` 失败时抛出 `std::system_error`。在其他平台上它什么都不做。PACE 在创建任何 socket 前都会调用它；
只有直接使用原始 Winsock 函数之前才需要自己调用。`initialize_winsock()` 是它的另一个名字。
`create_windows_socket` 用 `WSASocketW` 创建可重叠、不可继承的 socket，与 PACE 在 Windows 上创建的所有 socket
一样；失败时返回 `INVALID_SOCKET`。

## 错误

| 错误 | 时机 |
|:--|:--|
| `std::logic_error` | 嵌套、并发或重复调用 `run()`；在未运行本 context 的线程上使用 `local_state`、`find_local_state`、`on_cancel`、就绪等待、定时器和 `yield`；同一描述符同一方向上出现第二个挂起等待（`wait_ready` 占用两个方向） |
| `std::system_error(operation_canceled)` | 停止后调用 `spawn`、`post`、`call_at`、`on_cancel`、`local_state`、`find_local_state`；context 停止或调用 `cancel(fd)` 时的所有挂起等待 |
| 带令牌错误码的 `std::system_error` | 所属 task 的令牌被取消的等待（`operation_canceled`，截止时间到期则为 `timed_out`） |
| `std::system_error(bad_file_descriptor)` | 等待 `invalid_socket` |
| `std::system_error`（原生错误码） | 创建轮询器或注册失败 |
| `std::invalid_argument` | `post` 函数、定时器回调或取消回调为空；`local_state` 参数无效；`PACE_IO_BACKEND` 取值未知 |

**反应器错误策略。** 从根任务、投递回调、回调定时器、`on_cancel` 回调或轮询器本身逃出的异常会被记录（第一个有效），
随后 context 停止，并在排空后由 `run()` 重新抛出。context 已在停止时逃出的、等于 `operation_canceled` 的
`std::system_error` 是关闭的预期结果，不会被记录。

## 线程与并发

每个 context 恰好由一个线程运行，所有涉及其 socket、定时器和等待的操作都在该线程上进行。线程安全的入口是
`spawn`、`post`、`stop` 和 `stopped`；`current` 按线程区分。`run()` 之前的构造和返回之后的清理可以在任意一个线程上进行：
`call_at` 和 `call_after` 可在 `run()` 之前调用，`cancel(fd)`、`release_socket(fd)` 和句柄的 `cancel()`
在之前或之后都可以调用。

每个描述符同时最多支持一个读方向和一个写方向的挂起等待。

协程或回调会一直运行到挂起或返回。CPU 密集循环和阻塞调用会占住整个 context：其上的其他 task、定时器和 I/O
都无法推进，关闭也要等它们返回才能完成。事件循环每一轮对每类工作只处理有限的一批（见下方限制），
避免某个繁忙来源饿死其他工作。

## 生命周期与所有权

- context 必须比绑定到它的每个 socket、listener、定时器句柄的使用和挂起操作活得更久，并且不能在 `run()` 执行期间销毁。
  先 join 运行它的线程，再销毁 socket 和 listener，最后销毁 context。
- `spawn` 持有根任务直到它结束；context 销毁时仍挂起的根任务随之销毁。
- 就绪等待借用描述符；`local_state` 持有其键和值直到销毁。
- `timer_handle` 和 `cancellation_handle` 持有各自的回调，可以比 context 活得更久。

## 限制与默认值

| 项目 | 值 |
|:--|:--|
| 默认后端 | `io_backend::automatic`（见 [I/O 后端](io-backends.zh.md)） |
| 每个 context 的线程数 | 1 |
| 每个 context 的运行次数 | 1 |
| 每个描述符的挂起等待 | 读方向 1 个 + 写方向 1 个 |
| 每轮处理的工作 | 投递回调、就绪协程、到期定时器、到期回调定时器、取消回调以及新提交的 task/回调，各最多 256 个 |
| 每次轮询的就绪事件 | 256 |
| 轮询超时精度 | 毫秒，向上取整 |

## 参见

- [`pace::run`](run.zh.md)：为一个入口 task 创建新的 context；`this_context`、`sleep_for`、`yield`
- [`pace::runtime`](runtime.zh.md)：每个工作线程一个 context
- [I/O 后端](io-backends.zh.md)：选择轮询器
- [结构化并发](structured-concurrency.zh.md)：令牌、截止时间以及 `on_cancel` 的使用场景
- [教程：工作线程](../../tutorial/08-workers-and-threads.zh.md) ·
  [优雅关闭](../../tutorial/09-graceful-shutdown.zh.md)
- [执行模型](../../explanation/execution-model.zh.md)
