# QUIC

[简体中文](README.zh.md) · [Reference](../README.md)

## Overview

`pace::quic` provides QUIC v1 connections with independent, multiplexed streams, secured by TLS 1.3 with
mandatory peer verification on the client.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/quic.hpp>` | `pace_quic` | `--quic=y` (also enabled by `--http3=y`) |

The module has two interchangeable backends, selected when the build is configured. Both implement the same API
in `include/pace/quic/connection.hpp`, both define `PACE_HAS_QUIC`, and HTTP/3 runs on either.

| `--quic_backend=` | Packet processing, loss recovery, congestion and flow control | TLS and cryptography | Extra define |
|:--|:--|:--|:--|
| `ngtcp2` (default) | ngtcp2 1.26.0 | OpenSSL 3.5.7 | `PACE_QUIC_NGTCP2` |
| `openssl` | OpenSSL 3.5.7 native QUIC | OpenSSL 3.5.7 | none |

QUIC needs neither `--tls=y` nor the `pace_tls` target: it embeds its own TLS 1.3 handshake. The transport runs
on POSIX only (Linux, macOS, FreeBSD); configuring `--quic=y` or `--http3=y` on Windows or MinGW stops with a
configuration error. Addresses are numeric IPv4 or IPv6 endpoints; resolve names first with
[DNS](../dns/README.md). The C++ named module is `pace.quic`.

## Synopsis

```cpp
#include <pace/quic.hpp>
#include <pace/runtime.hpp>

#include <array>
#include <iostream>
#include <string>

pace::task<std::string> read_to_fin(pace::quic::stream& stream)
{
    std::array<char, 4096> buffer{};
    std::string bytes;
    while (auto count = co_await stream.read_some(buffer))
    {
        bytes.append(buffer.data(), count);
    }
    co_return bytes;
}

pace::task<void> echo_once(pace::quic::listener& listener)
{
    auto connection = co_await listener.accept();
    auto stream = co_await connection.accept_stream();
    co_await stream.write_all("echo: " + co_await read_to_fin(stream));
    stream.finish_write();
    while (stream.pending_write_bytes() != 0) // Keep the stream until the peer has acknowledged it.
    {
        co_await stream.wait_events();
    }
}

pace::task<void> ask(pace::quic::listener& listener, const pace::quic::context& client_config)
{
    co_await pace::with_task_group(
        [&](pace::task_group& group) -> pace::task<void>
        {
            auto server = group.spawn(echo_once(listener));
            auto connection = co_await pace::quic::connect(client_config, listener.local_endpoint(), "localhost");
            auto stream = co_await connection.open_stream();
            co_await stream.write_all("hello");
            stream.finish_write(); // FIN on our direction; the reply still arrives.
            std::cout << co_await read_to_fin(stream) << '\n';
            co_await server; // Keep the connection open until the server has its acknowledgement.
        }
    );
}

int main()
{
    pace::quic::context server_config(pace::quic::role::server);
    server_config.use_certificate_chain("localhost.pem", "localhost-key.pem");
    server_config.set_alpn_protocols({"example"});

    pace::quic::context client_config; // role::client, system trust store
    client_config.load_verify_file("ca.pem");
    client_config.set_alpn_protocols({"example"});

    pace::run(
        [&](pace::io_context& io) -> pace::task<void>
        {
            auto listener = pace::quic::listen(io, {"127.0.0.1", 0}, server_config);
            co_await ask(listener, client_config);
        }
    );
}
```

## Types

### `role` and `stream_type`

```cpp
enum class role { client, server };
enum class stream_type { bidirectional, unidirectional };
```

`role` selects which side of the handshake a `context` configures. `stream_type` selects the direction of a
stream opened with `connection::open_stream()`.

### `context`

```cpp
class context
{
public:
    explicit context(role mode = role::client);
    void load_verify_file(const std::string& path);
    void use_certificate_chain(const std::string& chain, const std::string& key);
    void set_alpn_protocols(const std::vector<std::string>& protocols);
};
```

The shared QUIC/TLS 1.3 configuration of one side. Copies are cheap and share one state.

- `context(mode)` creates a configuration for `mode`. TLS is fixed to version 1.3. A client loads the system trust
  store and always verifies the server; there is no insecure client mode and no client certificate (mutual TLS)
  in this API. The initial ALPN list is `{"pace-quic"}`.
- `load_verify_file(path)` adds the PEM certificates in `path` to the trusted roots, in addition to the system
  store.
- `use_certificate_chain(chain, key)` loads a PEM certificate chain and its private key for a server. The key must
  be an unencrypted PEM file; loading never prompts for a passphrase. A key that does not match the certificate
  is rejected.
- `set_alpn_protocols(protocols)` replaces the ALPN list. Each identifier is 1–255 bytes and the encoded list at
  most 65535 bytes; an empty list is rejected. The server selects the first protocol in its own list that the
  client also offers; no overlap fails the handshake. Read the result with `connection::selected_protocol()`.

Configure a context completely before the first `connection` or `listener` uses it, and do not change it while
any of them is alive.

### `connection`

```cpp
class connection
{
public:
    connection(io_context& io, const quic::context& config);
    connection(connection&&) noexcept;
    connection& operator=(connection&&) noexcept;
    ~connection();

    task<void> connect(udp::endpoint peer, std::string expected_name,
                       std::chrono::milliseconds timeout = std::chrono::seconds(10));
    task<stream> open_stream(stream_type type = stream_type::bidirectional);
    std::optional<stream> try_open_stream(stream_type type = stream_type::bidirectional);
    task<stream> accept_stream();
    std::optional<stream> try_accept_stream();
    task<void> wait_events();
    io_context& context() const noexcept;
    std::string selected_protocol() const;
    bool is_open() const noexcept;
    void close(std::uint64_t application_error = 0) noexcept;
};
```

A client connection, or a connection returned by `listener::accept()`. Move-only.

- `connection(io, config)` creates an unconnected client on `io`, bound to an ephemeral UDP port. `config` must be
  a client context.
- `connect(peer, expected_name, timeout)` performs the handshake with `peer` and verifies the server certificate
  against `expected_name`, which is independent of the network address. A DNS name is also sent as SNI and must
  match a DNS subject alternative name; an IPv4 or IPv6 literal must match an IP subject alternative name. An
  empty name, a name containing NUL and port 0 are rejected. `timeout` bounds the handshake; a nonpositive value
  allows one immediate attempt. A connection whose `connect()` failed is closed.
- `open_stream(type)` opens a stream, waiting while the peer's stream limit is reached. Opening a stream sends
  nothing: the peer sees it when its first data or FIN arrives.
- `accept_stream()` waits for the next stream the peer opened. A stream becomes visible when its first data or FIN
  arrives; arrival order across streams is not an ordering guarantee.
- `try_open_stream(type)` and `try_accept_stream()` do the same without suspending and return an empty optional
  when no stream can be opened or none is waiting.
- `wait_events()` suspends until there is transport progress on the connection. Wakeups may be spurious: recheck
  every stream of interest afterwards. It exists for protocol drivers that multiplex many streams with the `try_*`
  members.
- `selected_protocol()` returns the negotiated ALPN protocol, or an empty string before the handshake.
- `is_open()` is false after `close()`, after a connection failure and once the owning context has stopped.
- `close(application_error)` sends CONNECTION_CLOSE with `application_error`, best effort and immediately. It
  discards unacknowledged output, invalidates every stream of the connection and wakes their pending operations.
  There is no graceful drain. Closing one accepted connection leaves the listener's other connections running.

### `stream`

```cpp
class stream
{
public:
    stream(stream&&) noexcept;
    stream& operator=(stream&&) noexcept;
    ~stream();

    std::uint64_t id() const noexcept;
    bool is_unidirectional() const noexcept;
    bool is_local() const noexcept;
    io_context& context() const noexcept;
    task<std::size_t> read_some(std::span<char> buffer);
    task<void> write_all(std::string_view bytes);
    task<std::size_t> read_some(std::span<std::byte> buffer);
    task<void> write_all(std::span<const std::byte> bytes);
    task<void> wait_events();
    std::optional<std::size_t> try_read(std::span<char> buffer);
    std::size_t try_write(std::string_view bytes);
    std::uint64_t pending_write_bytes() const;
    void finish_write();
    void reset(std::uint64_t application_error = 0) noexcept;
};
```

One stream of a connection. Move-only. A stream handle keeps its connection's state alive.

- `id()` is the QUIC stream ID; `is_unidirectional()` and `is_local()` (this endpoint opened it) are derived from
  it. `context()` is the owning `io_context`.
- `read_some(buffer)` waits for at least one byte and returns the number read. It returns 0 only at the peer's FIN,
  or immediately for an empty buffer. A reset stream or failed connection is an error, not 0.
- `write_all(bytes)` completes when the transport has accepted every byte, applying flow-control backpressure. It
  does not mean the peer has received or acknowledged them. The bytes are borrowed: keep them alive until the
  task completes.
- The `std::byte` overloads behave the same.
- `try_read(buffer)` reads without suspending: an empty optional means no data yet, 0 means FIN, otherwise the
  number of bytes copied.
- `try_write(bytes)` queues as much as the flow-control windows allow without suspending and returns the count;
  0 means would-block for nonempty input. Retry the rest after progress.
- `wait_events()` suspends until there is transport progress on the connection; wakeups may be spurious.
- `pending_write_bytes()` is the number of bytes written but not yet acknowledged by the peer. Subtracting it from
  the total accepted by writes gives the acknowledged prefix in both backends. It does not cover the FIN, and an
  acknowledgement does not mean the application has read the data.
- `finish_write()` queues FIN on the sending direction. The receive direction stays usable.
- `reset(application_error)` abandons the stream with `application_error` and wakes its pending operations.

Reading the sending-only side of a unidirectional stream, writing its receive-only side, and writing after
`finish_write()` are errors.

### `listener`

```cpp
class listener
{
public:
    listener(io_context& io, udp::endpoint local, const quic::context& config);
    ~listener();

    task<connection> accept(std::chrono::milliseconds handshake_timeout = std::chrono::seconds(10));
    udp::endpoint local_endpoint() const;
    io_context& context() const noexcept;
    bool is_open() const noexcept;
    void close() noexcept;
};
```

A bound UDP socket that accepts QUIC connections. Not copyable or movable.

- `listener(io, local, config)` binds `local` immediately and starts receiving. `config` must be a server context
  with a certificate. An IPv6 endpoint is bound IPv6-only.
- `accept(handshake_timeout)` takes the next connection from the accept queue and completes its handshake. The
  timeout starts when the connection leaves the queue; it does not bound the wait for a new peer. A failed
  handshake (certificate, ALPN or deadline) throws, but the listener keeps accepting while `is_open()` is true.
- `local_endpoint()` returns the bound address, with the port the system chose for port 0.
- `is_open()` is false after `close()`, after an engine failure and once the owning context has stopped.
- `close()` cancels a pending `accept()` and invalidates every connection accepted on this listener. The
  destructor calls it.

## Functions

```cpp
listener listen(io_context& io, udp::endpoint local, const quic::context& configuration);
listener listen(udp::endpoint local, const quic::context& configuration);
```

Bind a listener, exactly like the `listener` constructor. The second form uses the current context and must be
called inside a running `io_context`.

```cpp
task<connection> connect(io_context& io, quic::context configuration, udp::endpoint peer,
                         std::string expected_name,
                         std::chrono::milliseconds timeout = std::chrono::seconds(10));
task<connection> connect(quic::context configuration, udp::endpoint peer, std::string expected_name,
                         std::chrono::milliseconds timeout = std::chrono::seconds(10));
```

Create a client `connection` and complete `connection::connect()`. The task holds its own copy of
`configuration` (a shared handle), so the caller's object need not outlive it. The timeout bounds the handshake
only, not later stream operations. The second form uses the current context. ALPN and the certificate identity
stay explicit: nothing is chosen for you.

The stream helpers of [`<pace/net/stream_io.hpp>`](../net/tcp.md) (`pace::write`, `pace::read_exact`) also accept a
`quic::stream`.

## Errors

| Condition | Exception |
|:--|:--|
| Invalid argument: role, empty or oversized ALPN, empty or NUL-containing name, port 0, wrong context role, stream type | `std::invalid_argument` |
| Scoped IPv6 destination with the `openssl` backend | `std::invalid_argument` |
| Overlapping operation of the same kind, call from another context, operation before the handshake, wrong stream direction, write after FIN | `std::logic_error` |
| Handshake deadline | `std::system_error` with `std::errc::timed_out` |
| Connection, listener or context closed or stopped; cancellation | `std::system_error` with `std::errc::operation_canceled` |
| Peer closed the connection, idle timeout | `std::system_error` with `std::errc::connection_aborted` |
| TLS or QUIC failure: untrusted or mismatched certificate, no common ALPN, key or trust file errors, protocol violation | `std::system_error` with `std::errc::protocol_error`; the message carries the backend's diagnostics |
| UDP socket failure | `std::system_error` with the system error |

A failure inside the shared UDP engine closes it and is reported to every operation on it. A server should catch
per-peer errors around `accept()` and the work on each connection so one peer cannot stop unrelated tasks.

## Threads and concurrency

- Every member, including the `try_*` members, runs on the thread running the owning `io_context`; a call from
  elsewhere throws `std::logic_error`. Construct and destroy handles on that thread too, or while the context is
  not running.
- Per stream, one read and one write may be outstanding at a time. Per connection, one `connect()`, one open and
  one accept may be outstanding. Per listener, one `accept()`. A second operation of the same kind throws
  `std::logic_error`.
- Distinct streams progress independently and concurrently. All connections of a listener, and all streams of
  a connection, share one UDP socket; `wait_events()` wakes on progress anywhere on it, so recheck state after
  each wakeup.
- Both backends are driven by PACE UDP readiness and cancellable `io_context` timers, with no polling thread.
  Library callbacks never resume application code directly: continuations are queued on the context.

## Lifetime and ownership

- The `io_context` must outlive every listener, connection and stream on it.
- A `stream` holds a shared reference to its connection's state, and a connection to its UDP engine. Destroying a
  `connection` object therefore does not invalidate its streams. The transport is released with the last handle;
  if delivery matters, wait until `pending_write_bytes()` is 0 before dropping it, and keep the connection open
  until the peer has finished.
- `connection::close()` invalidates every stream of the connection. `listener::close()` and the listener's
  destructor invalidate every connection accepted on it, so keep the listener alive while its connections are in
  use.
- `write_all()` and `read_some()` borrow their buffers until the task completes.
- A `context` is shared by the connections and listeners created from it; they keep it alive.

## Limits and defaults

| Setting | Value |
|:--|:--|
| Handshake timeout (`connect`, `accept`) | 10 s; nonpositive allows one immediate attempt |
| Initial ALPN list | `{"pace-quic"}` |
| ALPN identifier / encoded list | 1–255 bytes / at most 65535 bytes |
| TLS version | 1.3 only |
| Client trust | System store plus `load_verify_file()` |
| IPv6 listener | IPv6-only |
| Scoped IPv6 destinations | Preserved (`ngtcp2`); rejected (`openssl`) |

The `ngtcp2` backend uses these transport settings. The `openssl` backend uses OpenSSL's defaults.

| `ngtcp2` setting | Value |
|:--|:--|
| UDP payload size | 1200 bytes; PMTU discovery off |
| Per-stream flow-control window | 256 KiB |
| Connection flow-control window | 4 MiB |
| Incoming streams exposed at once | 64 per direction; credit returns as handles are released |
| `try_write()` per call | at most 16 KiB |
| Pending accept queue | 128 connections |
| Incomplete server handshake | expires after 10 s, even without an `accept()` |
| Idle timeout | 30 s |
| Packets per receive or send batch | 64 |
| Active connection migration | Disabled |

Not provided: 0-RTT, session resumption control, connection migration, QUIC DATAGRAM, custom congestion
control, client certificates and application-level graceful shutdown.

## See also

- [HTTP/3](../http3/README.md), which runs on this module
- [UDP](../net/udp.md) for `udp::endpoint`
- [DNS](../dns/README.md) to resolve names before connecting
- [Platform support](../platform-support.md) and [build options](../build-options.md)
- Tutorial: [QUIC and HTTP/3](../../tutorial/13-quic-and-http3.md)
- [Runnable example](../../../examples/tutorial_quic.cpp)
- [RFC 9000](https://www.rfc-editor.org/rfc/rfc9000), [ngtcp2 programmer's guide](https://nghttp2.org/ngtcp2/programmers-guide.html),
  [OpenSSL QUIC](https://docs.openssl.org/3.5/man7/openssl-quic/)
