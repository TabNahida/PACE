# 7. 发送和接收一个 UDP 数据报

[English](07-udp.md) · [上一章：关闭](06-shutdown.zh-CN.md) · [下一章：DNS/TLS](08-dns-tls.zh-CN.md)

TCP 章节要自己累计一条消息的字节。UDP 保留每次发送的数据报边界，
但不提供 TCP 那样的可靠有序传输。本章在本机发送一个数据报并打印它。
依赖 `pace_net`，完整程序如下：

```cpp
#include <pace/udp.hpp>
#include <pace/run.hpp>
#include <array>
#include <iostream>

pace::task<void> hello(pace::io_context& io) {
    pace::udp::socket receiver(io, {"127.0.0.1", 0});
    pace::udp::socket sender(io);
    co_await sender.send_to("Hello, UDP!", receiver.local_endpoint());
    std::array<char, 256> buffer{};
    auto packet = co_await receiver.receive_from(buffer);
    if (packet.truncated || std::string_view(buffer.data(), packet.size) != "Hello, UDP!")
        throw std::runtime_error("UDP message mismatch or truncation");
    std::cout << std::string_view(buffer.data(), packet.size) << '\n';
}
int main() {
    try { pace::run(hello); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
```

## 第一步：先让接收端有一个地址

构造 receiver 时指定回环地址和零端口，让系统分配可用端口，
再通过 `local_endpoint()` 取得真实地址。sender 可以不先 bind，发送时由系统选择本地端点。
两个 socket 都在入口协程内，入口返回前已经没有挂起的成员操作。

## 第二步：按一个数据报来接收

`send_to` 每次指定目的地址。`receive_from` 返回数据大小和来源端点，
回复时使用 `packet.sender`。注意 UDP 的零长度消息是有效消息，不是 EOF。
缓冲区要覆盖协议允许的最大数据报；如果 `packet.truncated` 为 true，说明缓冲区太小，报文剩余部分已被丢弃。
应拒绝这条不完整消息，不能把收到的前半段当成完整内容。

## 第三步：核对结果，再增加交互

构建运行 `my_application`，应输出 `Hello, UDP!` 并退出。
PACE 根目录的 `xmake run tutorial_udp` 是同一思路的配套练习。
可以先改消息长度，核对返回 size，再让 receiver 回复 `packet.sender`，最后由 sender 接收。

发送缓冲区要活到发送完成；同一 socket 不要并行发起两个 send 或两个 receive。
丢包、乱序、重试和消息身份由应用协议处理；需要可靠的多路流时看后面的 QUIC 章节。
[UDP 参考](../reference/udp.md)
