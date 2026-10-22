# UDP

Send one datagram and read it back locally:

```cpp
#include <pace/pace.hpp>
#include <array>
#include <iostream>

pace::task<void> hello(pace::io_context& io) {
    pace::udp::socket receiver(io, {"127.0.0.1", 0});
    pace::udp::socket sender(io);
    co_await sender.send_to("Hello, UDP!", receiver.local_endpoint());
    std::array<char, 256> buffer{};
    auto packet = co_await receiver.receive_from(buffer);
    std::cout << std::string_view(buffer.data(), packet.size) << '\n';
    io.stop();
}
int main() {
    pace::io_context io;
    io.spawn(hello(io));
    io.run();
}
```

Run the [checked-in example](../../examples/tutorial_udp.cpp):

```sh
xmake
xmake run tutorial_udp
```

No optional dependencies are needed. Use `packet.sender` to reply:

```cpp
co_await receiver.send_to("reply", packet.sender);
```

## API

Include `<pace/udp.hpp>`. All types are in `pace::udp`.

| API | Purpose |
|---|---|
| `endpoint{address, port}` | Numeric IPv4 address and UDP port; port 0 chooses a local ephemeral port |
| `socket(io)` | Create a nonblocking datagram socket; the OS binds it on first send |
| `socket(io, endpoint)` | Create and bind a socket |
| `bind(endpoint)` | Bind an unbound socket |
| `local_endpoint()` | Return the bound local address and port |
| `send_to(bytes, endpoint)` | Send one datagram; returns its size |
| `receive_from(buffer)` | Receive one datagram into a `span<char>` or `span<byte>` |
| `set_broadcast(bool = true)` | Enable or disable IPv4 broadcast sends |
| `close()` | Close the handle and cancel pending operations |
| `is_open()`, `context()`, `native_handle()` | Inspect ownership and the underlying handle |

`send_to` accepts `string_view` or `span<const byte>`. The socket is move-only;
move it only when it has no pending operations. Setup may happen before `run()`;
async operations run on its owning context. Keep the socket and borrowed buffers
alive until their operations finish. One send and one receive may be pending at
the same time; another operation in the same direction throws `logic_error`.

`receive_from` returns `datagram{size, sender, truncated}`:

- `size` is the number of bytes copied. **Zero is a valid empty datagram**, not EOF.
- `sender` is the source address and port.
- `truncated` means the buffer was too small. The rest of that datagram is discarded;
  the next receive starts with the next datagram.

An empty receive buffer throws `invalid_argument`. A payload over 65,507 bytes
throws `system_error(message_size)`; a network may impose a smaller effective
limit. Cancellation throws `system_error(operation_canceled)`. Other OS failures
throw `system_error`. UDP does not guarantee delivery, order, or uniqueness.

This API currently supports numeric IPv4 unicast and broadcast. DNS, IPv6 and
multicast membership are not exposed.

## 中文：先运行，再使用

上面的程序在本机发送并接收一条消息，直接运行 `xmake run tutorial_udp` 即可。
不需要 OpenSSL 或 nghttp2。`packet.sender` 是来源地址，可直接用于回复。

接收结果的 `size == 0` 表示空数据报，不表示连接结束。`truncated == true`
表示缓冲区不足，该数据报剩余部分已丢弃。UDP 保留消息边界，但不保证送达或顺序。

同一 socket 可同时挂起一次发送和一次接收；同方向重复操作会报错。协程操作在
socket 所属的 `io_context` 上执行，完成前要保留 socket 和缓冲区，不能移动它。
`close()` 或停止 context 会取消等待。当前地址支持数字形式的 IPv4；尚无 DNS、
IPv6 或加入组播组的接口。

[Tutorials](../tutorial/README.md) · [Reference index](README.md)
