# 3. 从 TCP 连接到完整消息

[English](03-tcp.md) · [上一章：任务](02-tasks-timers.zh-CN.md) · [下一章：HTTP](04-http.zh-CN.md)

目标是写一个回显服务器，再写一个能验证完整回复的客户端。沿用第一章的工程，
应用依赖改为 `pace_net`。先用数字地址 `127.0.0.1`，域名解析留到第八章。

## 第一步：把一个连接交给一个任务

新建 `tcp_server.cpp`：

```cpp
#include <pace/tcp.hpp>
#include <array>
#include <iostream>

pace::task<void> echo(pace::tcp::stream socket) {
    try {
        std::array<char, 4096> buffer{};
        while (const auto count = co_await socket.read_some(buffer))
            co_await socket.write_all(std::string_view(buffer.data(), count));
    } catch (const std::system_error& error) {
        std::cerr << "connection: " << error.what() << '\n';
    }
}

pace::task<void> accept_clients(pace::tcp::listener& listener) {
    for (;;) listener.context().spawn(echo(co_await listener.accept()));
}

int main() {
    try {
        pace::io_context io;
        pace::tcp::listener listener(io, {"127.0.0.1", 9090});
        io.spawn(accept_clients(listener));
        io.run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`listener` 构造时立即绑定端口，`accept` 等待一个连接。
拿到连接后，`spawn` 把 socket 按值转交给 `echo`，接收循环便能继续等待下一个客户端。
socket 由连接任务持有；listener 则在 `main` 中，一直活到 `io.run()` 返回。

读取结果是字节数，不是以零结尾的字符串。非空缓冲区读到零表示 EOF。
写入使用数组的一段 view，但要等 `write_all` 完成后才复用数组，所以等待期间数据有效。
连接异常在自己的任务内处理，不必让整个服务退出。
这个入门程序的 Ctrl-C 使用系统默认终止行为；第六章再补协作式关闭。

## 第二步：客户端先约定消息长度

新建 `tcp_client.cpp`：

```cpp
#include <pace/run.hpp>
#include <pace/tcp.hpp>
#include <iostream>
#include <span>
#include <string>

pace::task<std::string> exchange(pace::io_context& io) {
    pace::tcp::stream socket(io);
    co_await socket.connect({"127.0.0.1", 9090});
    const std::string message = "Hello from PACE";
    co_await socket.write_all(message);
    std::string reply(message.size(), '\0');
    std::size_t received = 0;
    while (received != reply.size()) {
        const auto n = co_await socket.read_some(
            std::span<char>(reply.data() + received, reply.size() - received));
        if (n == 0) throw std::runtime_error("EOF before the complete reply");
        received += n;
    }
    if (reply != message) throw std::runtime_error("echo mismatch");
    co_await socket.close_gracefully();
    co_return reply;
}

int main() {
    try { std::cout << pace::run(exchange) << '\n'; }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

这里故意保留接收循环：TCP 只有有序字节流，没有“一次 write 对应一次 read”的保证。
本例知道发出消息的长度，所以累计到同样长度再检查。真实协议需要长度前缀、
分隔符或其他分帧规则；即使本机一次 read 恰好读完，也不能依赖这个结果。

字符串和 socket 都在当前协程帧里，等待期间保持有效。
`close_gracefully` 先关闭发送方向，再进行有界 drain，最后关闭连接。
它不证明对方业务已经处理完消息。完成关闭后返回字符串，`pace::run` 才停止临时上下文。

## 第三步：分别运行，核对输出

在应用的 xmake.lua 中添加：

```lua
for _, name in ipairs({"tcp_server", "tcp_client"}) do
    target(name)
        set_kind("binary")
        add_files(name .. ".cpp")
        add_deps("pace_net")
end
```

执行 `xmake -y tcp_server tcp_client`。一个终端运行 `xmake run tcp_server`，
另一个运行 `xmake run tcp_client`，应看到 `Hello from PACE`。
再运行一次客户端，确认服务器还在接收连接。最后用 Ctrl-C 停止服务器。

也可以在 PACE 仓库根运行 `xmake run tutorial_tcp`：配套练习使用临时端口，
在同一进程内自动验证两端并退出。先理解上述步骤，再读[完整练习](../../examples/tutorial_tcp.cpp)
里的命令行选项和信号关闭逻辑。

## 第四步：改变一个条件再观察

两端一起改成 `::1` 可测试 IPv6；listener 端口设为零并读取 `local_endpoint()`
可让系统选测试端口。绑定 `0.0.0.0` 或 `::` 才会监听外部接口；IPv6 默认仅接收 IPv6。

一个 stream 可同时有一个读和一个写，不能有两个读或两个写。
关闭 socket 会取消等待，但不会等等待它的协程退栈；先销毁 C++ 对象再恢复成员协程
仍然是错误的。第六章会明确说明关闭、取消、等待返回、销毁之间的顺序。

[完整 TCP 参考](../reference/tcp.md)
