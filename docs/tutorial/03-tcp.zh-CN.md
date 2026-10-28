# 3. 一次调用完成 TCP 请求

[English](03-tcp.md) · [学习路线](README.zh-CN.md) · [下一章：HTTP](04-http.zh-CN.md)

使用 PACE 0.4.0 或更新版本和第一章的源码构建方式，链接 `pace_net`。
本章约定请求四字节、回复四字节，使用数字回环地址。

## 1. 先写客户端

保存为 `tcp_client.cpp`：

```cpp
#include <pace/tcp.hpp>
#include <iostream>

int main() {
    try {
        auto reply = pace::tcp::exchange({"127.0.0.1", 9090}, "ping", 4);
        std::cout << reply << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`exchange` 完成连接、写入完整请求、读取恰好四字节回复和关闭，直接返回自己持有数据的字符串。
调用会阻塞当前线程，默认总超时 30 秒；不需要 `run` 或协程。
要改为两秒，包含 `<chrono>`，把 `{.timeout = std::chrono::seconds(2)}` 作为第四个参数。

TCP 保留字节顺序，**不保留消息边界**。这里的 `4` 是约定的回复长度，不是“最大包大小”。
PACE 自动处理短读；提前遇到 EOF 会抛异常。两端必须使用同样的消息分帧规则。

## 2. 准备四字节回显服务端

保存为 `tcp_server.cpp`：

```cpp
#include <pace/run.hpp>
#include <pace/tcp.hpp>
#include <iostream>

pace::task<void> echo(pace::tcp::stream socket) {
    try {
        auto message = co_await pace::read_exact(socket, 4);
        co_await pace::write(socket, std::move(message));
    } catch (const std::system_error& error) {
        std::cerr << "connection: " << error.what() << '\n';
    }
}
pace::task<void> serve() {
    auto listener = pace::tcp::listen({"127.0.0.1", 9090});
    for (;;) pace::this_context().spawn(echo(co_await listener.accept()));
}
int main() {
    try { pace::run(serve); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`listen` 在运行中的 context 上绑定地址，`accept` 返回一条连接，`spawn` 将连接交给独立的
`echo` 任务，监听循环继续接收其他客户端。协程持有连接并处理连接错误。
服务端需要持续处理事件，因此使用 `run`；一次性客户端直接调用即可。

## 3. 构建并验证

在第一章的 xmake.lua 中添加：

```lua
for _, name in ipairs({"tcp_client", "tcp_server"}) do
    target(name)
        set_kind("binary")
        add_files(name .. ".cpp")
        add_deps("pace_net")
end
```

执行 `xmake -y tcp_client tcp_server`。在一个终端运行 `xmake run tcp_server`，
另一个终端运行 `xmake run tcp_client`，应输出 `ping` 后退出。再运行客户端仍能得到回复。
用 Ctrl-C 结束服务端。试着把两端长度改为六字节，内容改为 `hello!`；只改一端会破坏协议。

## 4. 需要异步组合时

在运行中的协程里，一次交换仍然只需一行：

```cpp
auto reply = co_await pace::tcp::async_exchange({"127.0.0.1", 9090}, "ping", 4);
```

需要在同一连接上做多次操作时，再自己保留 socket：

```cpp
auto socket = co_await pace::tcp::connect({"127.0.0.1", 9090});
co_await pace::write(socket, "ping");
auto reply = co_await pace::read_exact(socket, 4);
co_await socket.close_gracefully();
```

`pace::write` 在挂起期间持有字符串，`read_exact` 返回独立字符串，默认分配上限为 1 MiB。
socket 不能超出所属 context 的生命周期。异步助手不会自动增加直接客户端的总超时。

[直接客户端的限制与错误](../reference/direct-clients.zh-CN.md) ·
[缓冲区、IPv6 与并发](../reference/component-conveniences.zh-CN.md) ·
[调优](../reference/protocol-tuning.zh-CN.md) · [关闭](06-shutdown.zh-CN.md)

仓库中的 `xmake run tutorial_tcp` 是可以独立运行的异步回显练习。
