# 3. 连接、发送，再读取完整的 TCP 消息

[English](03-tcp.md) · [上一章：任务](02-tasks-timers.zh-CN.md) · [下一章：HTTP](04-http.zh-CN.md)

沿用第一章的**当前源码工程**，依赖换成 `pace_net`。先约定一个很小的协议：
客户端发送四个字节 `ping`，服务端原样回四个字节。数字地址让这个练习不依赖 DNS。

## 第一步：监听并接收连接

把下面的完整程序存为 `tcp_server.cpp`：

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

`listen` 在当前运行的 context 上立即绑定；`accept` 等待客户端。
`spawn` 将收到的 socket 交给独立的 `echo` 任务，所以监听循环能继续接收下一个连接。
`echo` 持有 socket，并在内部处理连接错误；`serve` 持有 listener。
`run` 持续运行到入口结束，这里的服务入口一直等待，所以用 Ctrl-C 结束本次练习。

## 第二步：连接并交换一条消息

把下面的完整程序存为 `tcp_client.cpp`：

```cpp
#include <pace/run.hpp>
#include <pace/tcp.hpp>
#include <iostream>

pace::task<std::string> exchange() {
    auto socket = co_await pace::tcp::connect({"127.0.0.1", 9090});
    co_await pace::write(socket, "ping");
    auto reply = co_await pace::read_exact(socket, 4);
    if (reply != "ping") throw std::runtime_error("echo mismatch");
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

`connect` 返回已连接的 socket。`pace::write` 持有传入字符串直到发送完成，
`read_exact` 返回指定长度的字符串。TCP 只有字节顺序，**没有消息边界**：
一次 read 不一定读满四字节，`read_exact` 会继续读取；提前 EOF 则抛异常。
本例由双方约定四字节，实际协议需要长度前缀等明确的分帧规则。

先 await 关闭，再退出入口。可以从 `run` 返回普通结果字符串，不能返回一个
仍依赖其内部 context 的 socket。

## 第三步：构建并核对

在第一章的 xmake.lua 中加入两个目标：

```lua
for _, name in ipairs({"tcp_server", "tcp_client"}) do
    target(name)
        set_kind("binary")
        add_files(name .. ".cpp")
        add_deps("pace_net")
end
```

执行 `xmake -y tcp_server tcp_client`，然后 `xmake run tcp_server`。
在另一个终端执行 `xmake run tcp_client`，应输出 `ping` 并退出。
再次运行客户端，服务器仍可接收。试着把两端长度和正文都换成六字节 `hello!`；
只改一端就破坏了协议约定。

仓库里的 `xmake run tutorial_tcp` 在临时端口检查变长回显，见[配套源码](../../examples/tutorial_tcp.cpp)。
缓冲区复用、IPv6、no-delay、分配上限与并发规则放在
[组件接口参考](../reference/component-conveniences.zh-CN.md)和[调优参考](../reference/protocol-tuning.zh-CN.md)。
[第六章](06-shutdown.zh-CN.md)再为持续运行的服务加入协作式关闭。
