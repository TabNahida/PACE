# 7. 一次调用完成 UDP 收发

[English](07-udp.md) · [学习路线](README.zh-CN.md) · [下一章：DNS/TLS](08-dns-tls.zh-CN.md)

使用 PACE 0.4.0 或更新版本和第一章的源码构建方式，链接 `pace_net`。
UDP 保留数据报边界，但不保证送达或顺序。

## 1. 先写直接客户端

保存为 `udp_client.cpp`：

```cpp
#include <pace/udp.hpp>
#include <iostream>

int main() {
    try {
        auto packet = pace::udp::exchange({"127.0.0.1", 9091}, "Hello, UDP!");
        if (packet.truncated) throw std::runtime_error("UDP reply truncated");
        std::cout << packet.data << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`exchange` 发送一个请求，返回来自该数字地址和端口的第一个回复。调用会阻塞当前线程，
默认总超时 30 秒，不需要配置事件循环。若要改为两秒，包含 `<chrono>`，
将 `{.timeout = std::chrono::seconds(2)}` 作为第三个参数。

结果持有 `data`，并提供 `sender` 和 `truncated`。解析前检查截断标记：超过接收容量的字节
已经丢失。空 `data` 是合法的零长度数据报，不是 EOF。请求不会自动重试。

## 2. 准备 UDP 回显服务

保存为 `udp_server.cpp`：

```cpp
#include <pace/udp.hpp>
#include <iostream>

pace::task<void> serve() {
    auto socket = pace::udp::bind({"127.0.0.1", 9091});
    for (;;) {
        auto packet = co_await socket.receive();
        if (!packet.truncated)
            co_await socket.send(std::move(packet.data), packet.sender);
    }
}
int main() {
    try { pace::run(serve); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`bind` 在当前 context 上创建并绑定 socket，`receive` 等待一个数据报，`send` 向它的来源回复。
本服务端丢弃截断的数据报。socket 在所有操作完成前保持存活，`run` 驱动持续运行的服务端。

## 3. 构建并验证

在第一章的 xmake.lua 中添加：

```lua
for _, name in ipairs({"udp_client", "udp_server"}) do
    target(name)
        set_kind("binary")
        add_files(name .. ".cpp")
        add_deps("pace_net")
end
```

执行 `xmake -y udp_client udp_server`，然后运行 `xmake run udp_server`。
另开终端执行 `xmake run udp_client`，应输出 `Hello, UDP!`。试着修改消息再运行。
用 Ctrl-C 停止服务端。停止服务端后再运行客户端，可以观察错误或超时；具体取决于系统
如何报告目标不可达。

## 4. 只发送，或组合异步操作

对于不需要回复的服务：

```cpp
auto sent = pace::udp::send({"127.0.0.1", 9091}, "notification");
```

返回值是本地 socket 接受的字节数，不代表对端已经收到。每次直接调用都使用新的 socket。
协程里的异步请求写法为：

```cpp
auto packet = co_await pace::udp::async_exchange({"127.0.0.1", 9091}, "hello");
```

异步版本不附加总超时。要复用 socket，或接受其他地址的回复，再像服务端一样在协程里使用
`udp::open()` / `udp::bind()`、`socket.send` 和 `socket.receive`。

[来源过滤、超时与错误](../reference/direct-clients.zh-CN.md) ·
[容量与所有权](../reference/component-conveniences.zh-CN.md) ·
[缓冲区复用与调优](../reference/protocol-tuning.zh-CN.md)

仓库内的 `xmake run tutorial_udp` 是一个独立的本地异步练习。
