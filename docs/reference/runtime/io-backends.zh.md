<!-- source-hash: 30b834b548e0 -->
# I/O 后端

[English](io-backends.md) · [运行时](README.zh.md)

## 概述

`io_backend` 指定 `io_context` 使用的就绪轮询器；选择哪个后端不影响任何 socket 或协议代码。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/runtime/io_backend.hpp>`（也可用 `<pace/runtime.hpp>`） | `pace_runtime` | 始终构建；Linux 上的 io_uring 需要 `--uring=y` |

所有后端都是**就绪**后端：它们报告 socket 可读或可写，随后由 PACE 自己执行非阻塞 socket 调用。io_uring 后端提交
`IORING_OP_POLL_ADD`/`POLL_REMOVE`；Windows IOCP 后端使用 `ProcessSocketNotifications` 和完成端口。
两者都不做基于完成的 I/O（io_uring 的 `SEND`/`RECV`、注册缓冲区、零拷贝，或重叠的 `WSARecv`/`WSASend`/`AcceptEx`），
更新的后端也不一定更快：请用自己的负载测量。

## 概要

```cpp
#include <pace/runtime.hpp>

#include <iostream>

int main()
{
    pace::io_context io;                         // automatic：PACE_IO_BACKEND，否则使用平台默认值
    std::cout << pace::backend_name(io.backend()) << '\n';
    pace::runtime workers(2, pace::io_backend::automatic);
    std::cout << pace::backend_name(workers.context(0).backend()) << '\n';
}
```

不修改代码，为使用自动默认值的程序选择 io_uring：

```sh
xmake f -m release --uring=y -y
xmake -y
PACE_IO_BACKEND=io_uring ./your-server
```

## 类型

### `io_backend`

```cpp
enum class io_backend
{
    automatic,
    epoll,
    kqueue,
    wsapoll,
    io_uring,
    iocp
};
```

| 值 | 平台 | 说明 |
|:--|:--|:--|
| `automatic` | 全部 | 若设置了 `PACE_IO_BACKEND` 则用它，否则用下面的平台默认值 |
| `epoll` | Linux | Linux 上的默认值 |
| `kqueue` | macOS、FreeBSD | 这些平台上的默认值 |
| `wsapoll` | Windows | IOCP 通知不可用时的后备；32 位构建总是使用它 |
| `io_uring` | 以 `--uring=y` 构建的 Linux | 只能显式选择 |
| `iocp` | 64 位 Windows，build 20348 或更新 | 可用时为 Windows 上的默认值 |

该值传给 `io_context` 和 `runtime` 的构造函数。`io_context::backend()` 报告实际使用的后端，不会是 `automatic`。

### 选择规则

1. 构造函数参数不是 `automatic` 时按原样使用。若该后端在本次构建或本系统中不可用，构造时抛出带
   `std::errc::not_supported` 的 `std::system_error`；对于显式选择，PACE 从不替换成其他后端。
2. 参数为 `automatic` 时读取环境变量 `PACE_IO_BACKEND`。接受的值是枚举名（`automatic`、`epoll`、`kqueue`、
   `wsapoll`、`io_uring`、`iocp`），所指的后端随后按第 1 步处理。其他任何值都会在构造时抛出 `std::invalid_argument`。
3. 否则使用平台默认值：Linux 上为 epoll，macOS 和 FreeBSD 上为 kqueue，Windows 上若系统支持 socket 通知则为 IOCP，
   否则为 WSAPoll。

## 函数

### `backend_name`

```cpp
inline const char* backend_name(io_backend value) noexcept;
```

`value` 的小写名称，与 `PACE_IO_BACKEND` 接受的字符串相同：`"epoll"`、`"kqueue"`、`"wsapoll"`、`"io_uring"`、
`"iocp"` 或 `"automatic"`。

## 错误

| 错误 | 时机 |
|:--|:--|
| `std::invalid_argument` | `PACE_IO_BACKEND` 是未知名称 |
| `std::system_error(not_supported)` | 显式选择的后端不可用：未使用 `--uring=y` 或不在 Linux 上的 io_uring、内核不支持 `IORING_FEAT_NODROP`、不在 Windows 上或 32 位构建或早于 build 20348 的 IOCP，或其他平台的原生后端 |
| `std::system_error`（原生错误码） | 内核拒绝创建轮询器，例如容器的 seccomp 策略或过低的锁定内存上限阻止了 io_uring 初始化 |

PACE 不会替你修改系统限制。

## 线程与并发

每个 context 拥有自己的轮询器：一个 epoll 或 kqueue 描述符、一个 io_uring 环或一个完成端口，外加一个唤醒通道
（Linux 上为 eventfd，kqueue 用户事件，Windows 上为 `PostQueuedCompletionStatus`），供其他线程上的 `spawn`、`post`
和 `stop` 使用。没有辅助线程轮询 socket。[`io_context`](io-context.zh.md#线程与并发) 每个 context 一个线程的规则
适用于所有后端。

## 生命周期与所有权

context 在 socket 第一次等待时注册它，并保留该注册直到 socket 被取消或释放。当你拥有原始 socket 时：

- `io_context::cancel(fd)` 中断其等待并保持它打开；之后可以再次等待它。
- 关闭它之前必须立即调用 `io_context::release_socket(fd)`。它取消等待并丢弃后端的注册，
  这样复用同一描述符编号的新 socket 不会与旧 socket 混淆。

PACE 的 socket 和 DNS 模块的解析器会自行完成这些操作。在内部，注册使用单调递增的整数键，从不使用指向 awaiter
或缓冲区的指针，因此迟到的通知（在取消或描述符复用之后到达）会被丢弃，而不会恢复已销毁的协程。
在 IOCP 上，取消后重新注册的 socket 会先等待移除被确认。

## 限制与默认值

| 项目 | 值 |
|:--|:--|
| 默认值 | `io_backend::automatic` |
| Linux 默认 | epoll，使用持久的水平触发兴趣 |
| macOS / FreeBSD 默认 | kqueue，一次性过滤器 |
| Windows 默认 | IOCP（64 位，build 20348+），否则 WSAPoll |
| io_uring 提交环 | 每个 context 256 项；这是提交的批大小，不是连接数上限 |
| io_uring 内核要求 | `IORING_FEAT_NODROP` |
| io_uring 轮询 | 一次性，因此同样的负载可能比 epoll 需要更多提交 |
| IOCP socket | Microsoft Winsock 提供程序的 socket；一次性水平触发注册 |
| 每次轮询的就绪事件 | 256 |

io_uring 不是 Linux 的默认值：在测量结果证明值得改变之前，它保持为显式选择。测试程序 `pace_iocp_diagnostic`
绕过 PACE 的轮询器直接调用 Windows API；IOCP 仅支持 64 位的限制就来自它的结果：在 WoW64 下，
所有 32 位注册都以 `WSAEINVAL` 失败。

## 参见

- [`io_context`](io-context.zh.md) 与 [`runtime`](runtime.zh.md)：接受后端参数的构造函数
- [构建选项](../build-options.zh.md)：`--uring`
- [平台支持](../platform-support.zh.md)
- [HTTP 网关性能](../../performance/http-gateway.zh.md)
- Microsoft：[Winsock socket 状态通知](https://learn.microsoft.com/en-us/windows/win32/winsock/winsock-socket-state-notifications)
  与 [`SOCK_NOTIFY_REGISTRATION`](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/ns-winsock2-sock_notify_registration)；
  [liburing](https://github.com/axboe/liburing)
