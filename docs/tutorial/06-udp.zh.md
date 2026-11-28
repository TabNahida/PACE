<!-- source-hash: cfd1573ac77d -->
# 6. UDP

[English](06-udp.md) · [学习路线](README.zh.md) · [上一章：TCP](05-tcp.zh.md)

UDP 保留数据报边界，但既不保证送达，也不保证顺序。本章构建一个 UDP 客户端和一个回显服务器，两者都链接 `pace_net`。

## 编写直接客户端

保存为 `udp_client.cpp`：

```cpp
#include <pace/net.hpp>
#include <iostream>

int main()
{
    try
    {
        auto packet = pace::udp::exchange({"127.0.0.1", 9091}, "Hello, UDP!");
        if (packet.truncated)
        {
            throw std::runtime_error("UDP reply truncated");
        }
        std::cout << packet.data << '\n';
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`exchange` 发送一个数据报，并返回来自同一地址和端口的第一个回复；其他发送方的数据报会被忽略。它阻塞当前线程，默认总预算为 30 秒，不需要任何事件循环设置。要改为两秒，包含 `<chrono>` 并把 `{.timeout = std::chrono::seconds(2)}` 作为第三个参数传入。

结果拥有 `data`，并给出 `sender` 和 `truncated`。使用消息之前先检查 `truncated`：超出接收容量（默认 64 KiB）的字节已经丢失。空的 `data` 是一个合法的零长度数据报，并不表示流结束。不会自动重试。

## 提供 UDP 回显服务

保存为 `udp_server.cpp`：

```cpp
#include <pace/net.hpp>
#include <iostream>

pace::task<void> serve()
{
    auto socket = pace::udp::bind({"127.0.0.1", 9091});
    for (;;)
    {
        auto packet = co_await socket.receive();
        if (!packet.truncated)
        {
            co_await socket.send(std::move(packet.data), packet.sender);
        }
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

`bind` 在正在运行的上下文上创建 socket。`receive` 等待一个数据报，`send` 回复给它的发送方。这个服务器丢弃被截断的数据报。只要循环在运行，socket 就保存在协程帧里；`run` 驱动服务器直到你停止它。

## 构建并试用

在 `xmake.lua` 中添加这些目标：

```lua
for _, name in ipairs({"udp_client", "udp_server"}) do
    target(name)
        set_kind("binary")
        add_files(name .. ".cpp")
        add_deps("pace_net")
end
```

运行 `xmake -y udp_client udp_server`，再运行 `xmake run udp_server`。在另一个终端运行 `xmake run udp_client`，预期输出 `Hello, UDP!`。换一个字符串试试。用 Ctrl-C 停止服务器后再运行客户端：取决于操作系统如何报告不可达的端口，你会看到连接错误，或者在预算用尽后看到超时。

## 只发送，或组合异步工作

对于不回复的服务：

```cpp
auto sent = pace::udp::send({"127.0.0.1", 9091}, "notification");
```

结果是本地 socket 接受的字节数，而不是送达回执。每次直接调用都使用一个新的 socket。在协程中，异步请求写作：

```cpp
auto packet = co_await pace::udp::async_exchange({"127.0.0.1", 9091}, "hello");
```

异步形式默认继承调用者的期限。传入 `{.timeout = 2s}` 可设置总预算。要复用 socket 或接受来自其他地址的回复，就在协程中使用 `udp::open()` 或 `udp::bind()`，配合 `socket.send` 和 `socket.receive`，正如服务器所做的那样。

配套程序 `xmake run tutorial_udp`（[源码](../../examples/tutorial_udp.cpp)）绑定一个接收端，从另一个用 `udp::open()` 打开的 socket 向它发送一个数据报，检查后输出 `Hello, UDP!`。

[UDP 参考](../reference/net/udp.zh.md) · [协议调优](../guides/protocol-tuning.zh.md)

下一章：[7. 结构化并发](07-structured-concurrency.zh.md)
