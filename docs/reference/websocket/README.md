# WebSocket

[简体中文](README.zh.md) · [Reference](../README.md)

## Overview

`pace::websocket` provides RFC 6455 WebSocket clients and servers over TCP (`ws`) and, with TLS enabled, over
verified TLS (`wss`): the HTTP/1.1 Upgrade handshake, text and binary messages, fragmentation, subprotocol
selection, ping/pong and the closing handshake.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/websocket.hpp>` | `pace_websocket` | `--websocket=y` (on by default); `secure_stream` also needs `--tls=y` |

The module defines `PACE_HAS_WEBSOCKET`. Plain `ws` has no OpenSSL dependency and does not use the HTTP/1 module:
a WebSocket stream parses its own Upgrade request or response. Nonces and masking keys come from the operating
system's cryptographic random generator. The C++ named module is `pace.websocket`.

## Synopsis

```cpp
#include <pace/runtime.hpp>
#include <pace/websocket.hpp>

#include <iostream>

namespace ws = pace::websocket;

pace::task<void> echo(pace::tcp::stream transport)
{
    auto socket = co_await ws::accept(
        std::move(transport),
        {"echo"},
        [](const ws::upgrade_request& request) { return request.target == "/echo"; } // Your Origin/auth policy.
    );
    while (auto message = co_await socket.receive()) // nullopt once the peer has closed.
    {
        co_await socket.send(std::move(*message));
    }
}

int main()
{
    pace::run(
        [](pace::io_context& io) -> pace::task<void>
        {
            pace::tcp::listener listener(io, {"127.0.0.1", 0});
            co_await pace::with_task_group(
                [&](pace::task_group& group) -> pace::task<void>
                {
                    auto server = group.spawn(
                        [&]() -> pace::task<void> { co_await echo(co_await listener.accept()); }
                    );

                    auto socket = co_await ws::connect(listener.local_endpoint(), "localhost", "/echo", {"echo"});
                    co_await socket.send_text("Hello from PACE");
                    if (auto reply = co_await socket.receive())
                    {
                        std::cout << reply->data << " (" << socket.selected_protocol() << ")\n";
                    }
                    co_await socket.close_gracefully();
                    co_await server;
                }
            );
        }
    );
}
```

## Types

### `role`, `message_type` and `message`

```cpp
enum class role { client, server };
enum class message_type { text, binary };

struct message
{
    message_type type = message_type::text;
    std::string data;
};
```

`role` selects which side of the Upgrade handshake a stream performs. A `message` is one complete application
message, reassembled from its fragments; `data` may hold any bytes, including NUL. Text must be valid UTF-8.

### `limits`

```cpp
struct limits
{
    std::size_t max_header_bytes = std::size_t{16} * 1024;
    std::size_t max_frame_bytes = std::size_t{64} * 1024;
    std::size_t max_message_bytes = std::size_t{1024} * 1024;
    std::size_t max_fragments = 1024;
    std::size_t max_control_frames = 1024;
};
```

Per-stream bounds, passed to the constructor or the helper functions.

- `max_header_bytes`: the HTTP Upgrade request or response head. Header parsing also accepts at most 100 fields.
- `max_frame_bytes`: payload of one data frame, sent or received.
- `max_message_bytes`: payload of one reassembled message.
- `max_fragments`: data frames in one message.
- `max_control_frames`: ping, pong and close frames handled by one `receive()` call, which bounds control-frame
  floods.

`max_header_bytes` must be 64 bytes to 1 MiB; the others must be nonzero, and `max_frame_bytes` may not exceed
`max_message_bytes`. Sizes are checked before the payload is allocated.

### `upgrade_request`

```cpp
struct upgrade_request
{
    std::string target;
    std::string host;
    std::string origin;
    std::vector<std::string> protocols;
};
```

What a client asked for, as returned by a server's `accept()` and passed to its authorization callback: the
request target (path and query), the `Host` and `Origin` values and the offered subprotocols in the client's order.
Other request headers are not exposed.

### `protocol_error`

```cpp
class protocol_error : public std::runtime_error
{
public:
    protocol_error(std::uint16_t code, const std::string& message);
    std::uint16_t close_code() const noexcept;
};
```

A WebSocket protocol violation, by the peer or in the upgrade exchange. `close_code()` is the RFC 6455 status that
describes it: 1002 (protocol error), 1007 (invalid UTF-8), 1008 (policy: control-frame flood, or an upgrade the
application rejected) or 1009 (message, frame or fragment limit).

### `basic_stream`, `stream` and `secure_stream`

```cpp
template <class Transport>
class basic_stream
{
public:
    explicit basic_stream(Transport transport, role mode, limits bounds = {});
    ~basic_stream();
    basic_stream(basic_stream&&) noexcept;
    basic_stream& operator=(basic_stream&&) noexcept;

    task<void> handshake(std::string host, std::string target = "/", std::vector<std::string> protocols = {});
    task<upgrade_request> accept(std::vector<std::string> supported_protocols = {},
                                 std::function<bool(const upgrade_request&)> authorize = {});

    task<std::optional<message>> receive();
    task<void> send(message value, std::size_t fragment_bytes = 0);
    task<void> send_text(std::string text, std::size_t fragment_bytes = 0);
    task<void> send_binary(std::string bytes, std::size_t fragment_bytes = 0);
    task<void> ping(std::string payload = {});
    task<void> pong(std::string payload = {});
    task<void> send_close(std::uint16_t code = 1000, std::string reason = {});
    task<bool> close_gracefully(std::uint16_t code = 1000, std::string reason = {},
                                std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
    void close() noexcept;

    bool is_open() const noexcept;
    io_context& context() const noexcept;
    std::string selected_protocol() const;
    std::optional<std::uint16_t> peer_close_code() const;
    std::string peer_close_reason() const;
};

using stream = basic_stream<tcp::stream>;
using secure_stream = basic_stream<tls::stream>; // with --tls=y
```

A WebSocket over an owned transport: a connected `tcp::stream`, or a `tls::stream` whose handshake has completed
and negotiated no ALPN or `http/1.1` (`h2` is rejected: WebSocket over HTTP/2 is not implemented). Give the
transport to the WebSocket before reading any HTTP bytes from it: the stream parses the Upgrade itself, including
frame bytes that arrive with the headers. There is no conversion from a request already parsed by an HTTP server.

- `basic_stream(transport, mode, bounds)` takes ownership of `transport` and validates `bounds`.
- `handshake(host, target, protocols)` performs the client Upgrade. `host` is the `Host` value, independent of the
  numeric address the transport is connected to; `target` is an origin-form path and query starting with `/`,
  without a fragment; `protocols` are the offered subprotocols. The client checks the exact accept key, the
  `Connection` and `Upgrade` tokens, that the selected subprotocol was offered, and that no extension was
  negotiated.
- `accept(supported_protocols, authorize)` performs the server Upgrade and returns the `upgrade_request`. The
  server selects the first of its `supported_protocols` that the client offered; with no overlap it selects none,
  as RFC 6455 allows, so check `selected_protocol()` if your application requires one. `authorize`, if set, runs
  synchronously and must not block; returning false answers 403. An invalid request is answered with 400, and a
  WebSocket version other than 13 with 426 and the supported version.
- `receive()` returns the next complete message. It reassembles fragments, validates UTF-8 after reassembly,
  answers ping and consumes pong, also between the fragments of a message. After a peer close it sends the close
  reply, closes the transport and returns an empty optional.
- `send(value, fragment_bytes)` sends one message in frames of `fragment_bytes`; 0, or a value larger than
  `max_frame_bytes`, means `max_frame_bytes`. Text is checked for UTF-8. The data is owned across suspension.
  `send_text` and `send_binary` are shorthands.
- `ping(payload)` and `pong(payload)` send control frames of at most 125 bytes. Incoming pings are answered by
  `receive()`; there is no pong callback or heartbeat timer.
- `send_close(code, reason)` starts the closing handshake; then call `receive()` to process the peer's reply. The
  reason must be valid UTF-8 of at most 123 bytes. No data may be sent afterwards.
- `close_gracefully(code, reason, timeout)` sends close (unless already sent), discards incoming messages until the
  peer's close frame arrives, and closes the transport in every case. It returns true if the peer's close frame
  arrived and false if `timeout` expired first; other errors propagate. The timeout is cooperative and its timer is
  released as soon as the exchange ends.
- `close()` and the destructor close the transport at once, without a closing handshake.
- `selected_protocol()` is the negotiated subprotocol or an empty string. `peer_close_code()` and
  `peer_close_reason()` report the peer's close frame; the code is empty if none arrived or it carried no status.

## Functions

```cpp
template <class Transport>
task<basic_stream<Transport>> upgrade(Transport transport, std::string authority, std::string target = "/",
                                      std::vector<std::string> protocols = {}, limits bounds = {});
```

Client: wrap a connected TCP or handshaken TLS `transport` and perform `handshake()`. `authority` is the `Host`
value, including the port when needed, not a URL. Use this form for `wss` after the TLS handshake.

```cpp
template <class Transport>
task<basic_stream<Transport>> accept(Transport transport, std::vector<std::string> protocols = {},
                                     std::function<bool(const upgrade_request&)> authorize = {},
                                     limits bounds = {});
```

Server: wrap an accepted TCP or handshaken TLS `transport` and perform `basic_stream::accept()`. An empty
`authorize` accepts every request. It returns only the stream; use the member `accept()` when you also need the
`upgrade_request`.

```cpp
task<stream> connect(io_context& io, tcp::endpoint peer, std::string authority, std::string target = "/",
                     std::vector<std::string> protocols = {}, limits bounds = {});
task<stream> connect(tcp::endpoint peer, std::string authority, std::string target = "/",
                     std::vector<std::string> protocols = {}, limits bounds = {});
```

Client: connect TCP to the numeric `peer`, then `upgrade()`. The second form uses the current context.

The helpers own their transport and every argument; references captured by the `authorize` callback stay
borrowed. They do not parse `ws://` URLs or resolve names; use [DNS](../dns/README.md) for that.

## Errors

| Condition | Exception |
|:--|:--|
| Invalid role or limits, invalid host, target or subprotocol list, Upgrade request over `max_header_bytes`, invalid outgoing message (UTF-8, size, fragment count), control payload over 125 bytes, invalid close code or reason | `std::invalid_argument` |
| Peer protocol violation, invalid or rejected Upgrade, end of input without a close frame | `websocket::protocol_error` |
| Overlapping operation, call from another context, operation before the Upgrade, handshake in the wrong role or twice, send after close, use of a moved-from stream, TLS transport with ALPN other than `http/1.1` | `std::logic_error` |
| Cancellation, or the context stopped | `std::system_error` with `std::errc::operation_canceled` |
| Transport already closed | `std::system_error` with `std::errc::bad_file_descriptor` |
| Socket and TLS errors | `std::system_error` from the transport |

The stream rejects wrong masking direction, reserved bits and opcodes, extensions, non-minimal length encodings,
unexpected continuations, fragmented or oversized control frames, invalid UTF-8, invalid close codes and
oversized messages. After a protocol error during an open session it sends the matching close frame (best effort)
and closes the transport. Any failure during an active operation closes the transport, except an overlap
rejection, which leaves the running operation alone.

## Threads and concurrency

- **One operation at a time per stream**, sends and receive included, as well as the handshake and the closing
  calls. Separate reader and writer coroutines must not share a stream; a second operation throws
  `std::logic_error`. The stream suits request/response and echo exchanges, not server pushes during a pending
  `receive()`.
- Every member runs on the transport's context. Call `close()` on that thread too, or while the context is not
  running.

## Lifetime and ownership

- The stream owns its transport and closes it when destroyed. Keep the stream and its context alive until every
  operation has completed.
- Move a stream only while it is idle. A moved-from stream supports only destruction, assignment, `close()` and
  `is_open()`.
- Outgoing data is owned by the operation; received messages are owned by the caller.
- The `authorize` callback is stored for the duration of `accept()`; what it captures by reference must outlive it.

## Limits and defaults

| Setting | Default | Bounds |
|:--|--:|:--|
| `max_header_bytes` | 16 KiB | 64 bytes – 1 MiB |
| `max_frame_bytes` | 64 KiB | nonzero, ≤ `max_message_bytes` |
| `max_message_bytes` | 1 MiB | nonzero |
| `max_fragments` | 1024 per message | nonzero |
| `max_control_frames` | 1024 per `receive()` | nonzero |
| Upgrade header fields | 100 | fixed |
| Control frame payload | 125 bytes | fixed |
| Close reason | 123 bytes of UTF-8 | fixed |
| `close_gracefully()` timeout | 100 ms | |
| Default close code | 1000 | |
| Receive buffer | 8 KiB per stream | fixed |

Not provided: `permessage-deflate` compression, WebSocket over HTTP/2 (RFC 8441), URL parsing, automatic
reconnection, streaming delivery of partial messages, and simultaneous reading and writing.

## See also

- [TLS](../tls/README.md) for `secure_stream`
- [TCP](../net/tcp.md), [HTTP/1](../http1/README.md)
- Tutorial: [WebSocket](../../tutorial/11-websocket.md)
- [Runnable example](../../../examples/tutorial_websocket.cpp)
- [RFC 6455](https://www.rfc-editor.org/rfc/rfc6455)
