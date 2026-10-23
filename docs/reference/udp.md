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
| `endpoint{address, port, family, scope_id}` | Numeric IPv4/IPv6 and UDP port; the last two fields are optional |
| `socket(io)` | Create a nonblocking socket; first bind/send selects its family |
| `socket(io, pace::address_family)` | Create a socket with an explicit IPv4 or IPv6 family |
| `socket(io, endpoint, bind_options = {})` | Create and bind a socket |
| `bind(endpoint, bind_options = {})` | Bind an unbound socket |
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

## IPv6 and dual-stack policy

`endpoint` retains its original `address` and `port` fields, followed by
`pace::address_family family = pace::address_family::unspecified` and
`uint32_t scope_id = 0`. Existing `{address, port}` initializers remain valid.
An unspecified input family is inferred from the numeric address; an explicit
`ipv4` or `ipv6` must match. Local and sender endpoints report the concrete
family. `::1` is IPv6 loopback; `::` is the IPv6 wildcard. IPv6 addresses are
unbracketed. Hostnames and malformed addresses throw `invalid_argument`.
Resolve hostnames with the separate [DNS extension](dns.md).

Use a numeric interface index in `scope_id` for scoped IPv6 addresses. Native
sender/local inspection preserves it separately from the address string.
`%zone` strings, including numeric suffixes, are rejected; for example, use
`{"fe80::1", port, pace::address_family::ipv6, 3}` instead of `fe80::1%3`.
Nonzero scope on IPv4 is invalid. Link-local reachability depends on the selected
host interface; PACE does not resolve interface names.

An IPv6 socket defaults to `IPV6_V6ONLY=1`, independent of the OS default. To
receive both families on one wildcard socket, use:

```cpp
pace::udp::socket receiver(io, {"::", 0}, {.ipv6_only = false});
```

This explicitly sets `IPV6_V6ONLY=0` before bind; native option failures propagate.
`ipv6_only=false` is invalid on an IPv4 endpoint. IPv4 senders appear as mapped
IPv6 endpoints, such as `::ffff:127.0.0.1`, and can be passed directly to
`send_to` for a reply. On a dual-stack socket, `send_to` also maps an ordinary
IPv4 destination to the required native IPv6 form. IPv6-only sockets reject
IPv4 destinations. There is no multicast membership API, and IPv6 has no
broadcast: `set_broadcast` is only supported on IPv4 sockets.

The default constructor initially owns an IPv4 handle. Before the first bind,
send or receive, it can replace that handle to select IPv6. After the family is
selected, it cannot change. Calling `set_broadcast` also selects IPv4. Use an
explicit family constructor when applying native socket options before setup;
it keeps the handle and rejects a mismatched family. Bind requires an idle
socket. Auto-selection does not change the socket's context or ownership rules.

## 中文：先运行，再使用

上面的程序在本机发送并接收一条消息，直接运行 `xmake run tutorial_udp` 即可。
不需要 OpenSSL 或 nghttp2。`packet.sender` 是来源地址，可直接用于回复。

接收结果的 `size == 0` 表示空数据报，不表示连接结束。`truncated == true`
表示缓冲区不足，该数据报剩余部分已丢弃。UDP 保留消息边界，但不保证送达或顺序。

同一 socket 可同时挂起一次发送和一次接收；同方向重复操作会报错。协程操作在
socket 所属的 `io_context` 上执行，完成前要保留 socket 和缓冲区，不能移动它。
`close()` 或停止 context 会取消等待。地址支持数字形式的 IPv4/IPv6，主机名可先用
独立的 [DNS 扩展](dns.md)解析。IPv6 默认只接收 IPv6；在 `::` 上绑定时显式设置
`{.ipv6_only = false}` 可启用双栈。IPv4 来源保留为 `::ffff:...` 映射地址，方便直接回复。
作用域使用独立的数字 `scope_id`，不接受 `%接口名` 或 `%数字` 后缀。尚无加入组播组的接口。

[Tutorials](../tutorial/README.md) · [Reference index](README.md)
