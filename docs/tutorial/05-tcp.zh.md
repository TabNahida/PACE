<!-- source-hash: e3c349760859 -->
# 5. TCP

[English](05-tcp.md) · [学习路线](README.zh.md) · [上一章：任务与定时器](04-tasks-and-timers.zh.md)

本章构建一个 TCP 客户端和一个回显服务器。协议是四字节请求、四字节回复，使用数字形式的回环地址。两个程序都链接 `pace_net`。

## 先写客户端

保存为 `tcp_client.cpp`：

```cpp
#include <pace/net.hpp>
#include <iostream>

int main()
{
    try
    {
        auto reply = pace::tcp::exchange({"127.0.0.1", 9090}, "ping", 4);
        std::cout << reply << '\n';
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`exchange` 建立连接，写出完整请求，恰好读取四个字节，然后关闭连接。它返回一个自有的字符串，并阻塞当前线程，默认总预算为 30 秒。不需要 `run` 或协程。要换一个预算，包含 `<chrono>` 并把 `{.timeout = std::chrono::seconds(2)}` 作为第四个参数传入。

TCP 传递的是字节流，**不是消息**。最后一个参数是回复的确切长度，而不是上限。PACE 会在短读之后继续读取，如果对端在四个字节到齐前关闭则抛出异常。双方必须就消息如何分帧达成一致。

## 提供四字节回显服务

保存为 `tcp_server.cpp`：

```cpp
#include <pace/net.hpp>
#include <pace/runtime.hpp>
#include <iostream>

pace::task<void> echo(pace::tcp::stream socket)
{
    try
    {
        auto message = co_await pace::read_exact(socket, 4);
        co_await pace::write(socket, std::move(message));
    }
    catch (const std::system_error& error)
    {
        std::cerr << "connection: " << error.what() << '\n';
    }
}

pace::task<void> serve()
{
    auto listener = pace::tcp::listen({"127.0.0.1", 9090});
    for (;;)
    {
        pace::this_context().spawn(echo(co_await listener.accept()));
    }
}

int main()
{
    try
    {
        pace::run(serve);
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`listen` 在正在运行的上下文上绑定地址。每次 `accept` 返回一个已连接的流，`spawn` 把它交给一个独立的 `echo` 任务，监听循环随即可以接受下一个连接。`echo` 协程拥有自己的流，并自行处理该连接的错误。服务器使用 `run`，因为它要持续处理事件；像上面那样的一次性客户端则不需要。

## 构建并试用两端

在 `xmake.lua` 中添加这些目标：

```lua
for _, name in ipairs({"tcp_client", "tcp_server"}) do
    target(name)
        set_kind("binary")
        add_files(name .. ".cpp")
        add_deps("pace_net")
end
```

用 `xmake -y tcp_client tcp_server` 构建。在一个终端运行 `xmake run tcp_server`，再在另一个终端运行 `xmake run tcp_client`。预期输出 `ping`，客户端随后退出。再次运行客户端：服务器仍在监听。用 Ctrl-C 停止服务器。

把两边的长度都改为六，载荷改为 `hello!`，就得到六字节的交换。只改一边会破坏协议：取决于改的是哪一边，客户端会报告连接提前关闭，或者一直等到超时。

## 需要时组合异步工作

在正在运行的协程中，同样的一次性交换写作：

```cpp
auto reply = co_await pace::tcp::async_exchange({"127.0.0.1", 9090}, "ping", 4);
```

要在一个连接上进行多次交换，就自己保留 socket：

```cpp
auto socket = co_await pace::tcp::connect({"127.0.0.1", 9090});
co_await pace::write(socket, "ping");
auto reply = co_await pace::read_exact(socket, 4);
co_await socket.close_gracefully();
```

`pace::write` 在等待期间拥有它的字符串。`read_exact` 返回自有的字符串，除非传入更大的上限，否则拒绝超过 1 MiB 的长度。socket 的生命周期不能超过它的上下文。异步调用不附加阻塞调用的 30 秒预算。

配套程序 `xmake run tutorial_tcp`（[源码](../../examples/tutorial_tcp.cpp)）在一个进程里运行完整的回显服务器和客户端，输出 `TCP echo verified: Hello from PACE`。它的回显循环用 `read_some` 读入可复用的缓冲区，再把收到的内容写回；它的接受循环把监听器关闭视为循环结束。`xmake run tutorial_tcp serve` 在 9090 端口上保持服务器直到按下 Ctrl-C；`xmake run tutorial_tcp client` 连接这个服务器。

[TCP 参考](../reference/net/tcp.zh.md) · [协议调优](../guides/protocol-tuning.zh.md) · [有序关闭](09-graceful-shutdown.zh.md)

下一章：[6. UDP](06-udp.zh.md)
