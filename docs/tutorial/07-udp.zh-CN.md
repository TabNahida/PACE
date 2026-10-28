# 7. 直接发送和接收 UDP 消息

[English](07-udp.md) · [上一章：关闭](06-shutdown.zh-CN.md) · [下一章：DNS/TLS](08-dns-tls.zh-CN.md)

沿用第一章的当前源码，依赖 `pace_net`。UDP 保留每次发送的消息边界，
但不保证送达或顺序。先把 main.cpp 换成这个完整的本地练习：

```cpp
#include <pace/udp.hpp>
#include <pace/run.hpp>
#include <iostream>

pace::task<void> hello() {
    auto receiver = pace::udp::bind({"127.0.0.1", 0});
    auto sender = pace::udp::open();
    co_await sender.send("Hello, UDP!", receiver.local_endpoint());
    auto packet = co_await receiver.receive();
    if (packet.truncated || packet.data != "Hello, UDP!")
        throw std::runtime_error("UDP message mismatch or truncation");
    std::cout << packet.data << '\n';
}
int main() {
    try { pace::run(hello); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
```

## 第一步：给接收方一个地址

`udp::bind` 返回已绑定的 socket。端口 0 让系统选择空闲端口，
`local_endpoint()` 取得实际地址。`udp::open()` 创建未绑定的发送方，
第一次发送时选择本地地址。在 `pace::run` 的任务内无需手工传递 context。

## 第二步：直接取得消息内容

`send` 在等待期间持有字符串；`receive` 返回 `packet.data`、`packet.sender`
和 `packet.truncated`，不必先准备数组再按字节数转换字符串。
先检查截断再解释正文：过大的数据报会丢弃剩余字节。
空的 data 仍是有效的零长度数据报，不代表 EOF。

## 第三步：增加一条回复

构建运行 `my_application`，应输出 `Hello, UDP!` 并退出。随后在打印后插入：

```cpp
co_await receiver.send(packet.data, packet.sender);
auto reply = co_await sender.receive();
if (reply.truncated || reply.data != "Hello, UDP!")
    throw std::runtime_error("UDP reply mismatch");
```

再次运行，发送方会收到按原消息来源地址返回的回复。
两个 socket 都活到各自 await 完成。连接远程服务时，丢包处理由应用协议定义；
这个本地练习没有增加自动重试。

仓库的 `xmake run tutorial_udp` 对应第一个练习。
[容量、取消与所有权](../reference/component-conveniences.zh-CN.md#udp) ·
[复用缓冲区与调优](../reference/protocol-tuning.zh-CN.md#tcp-与-udp)
