# Errors

[简体中文](errors.zh.md) · [Reference](README.md)

## Overview

PACE reports failures with exceptions by default. Awaiting a task rethrows whatever the task threw, so errors travel through
`co_await` chains like ordinary C++ exceptions. Each module page lists the errors of its own operations; this page
describes the categories they all use.

| Header | Target | Build option |
|:--|:--|:--|
| standard `<system_error>`, `<stdexcept>`; `<pace/http.hpp>`, `<pace/websocket.hpp>`, `<pace/dns.hpp>` for the module-specific types | — | — |

## Types

| Exception | Means | Typical causes |
|:--|:--|:--|
| `std::system_error` | The operation failed at run time | A socket error from the OS (`connection_reset`, `broken_pipe`, …); cancellation; a deadline; a TLS handshake or verification failure; a DNS lookup failure |
| `std::invalid_argument` | An argument is malformed | An unparsable address, endpoint or URL; an invalid header name or value; a limit or option out of range |
| `std::logic_error` | The call is not allowed in the object's current state | An operation from a thread other than its context's; a second concurrent operation where one is allowed; using a stream before its handshake; a blocking client called inside a running context; a nested `pace::run` |
| `std::length_error` | A configured limit was exceeded | A body, header block or stream count above its `limits` value |
| `std::overflow_error` | A size computation would overflow | Lengths near the limits of `std::size_t` |
| `pace::http::protocol_error` | The HTTP peer violated the protocol | A malformed or truncated response seen by a client; the connection is closed |
| `pace::websocket::protocol_error` | The WebSocket peer violated the protocol | `close_code()` returns the close code sent to the peer |
| `std::runtime_error` | Other failures inside a protocol implementation | HTTP/2 session errors such as a peer `GOAWAY` or stream reset |
| `std::bad_alloc` | Memory allocation failed | Can propagate from any operation that allocates |

`std::logic_error` and `std::invalid_argument` mean the program is wrong: fix the call rather than retry it.
`std::system_error` and the protocol errors describe the network and the peer, and are expected in correct
programs.

## Result-returning I/O

Pass `pace::as_result` as the first argument to TCP `connect` (member or free function), `accept`,
`read_some` and `write_all`, or UDP `send_to` and `receive_from` (including the packet-info overloads).
These operations return `task<pace::result<T>>`. Native socket failures and readiness cancellation go
directly into the result. The socket ownership, buffer lifetime, and single-operation rules apply to both API forms.

```cpp
auto incoming = co_await listener.accept(pace::as_result);
if (!incoming) {
    log_error(incoming.error());
    co_return;
}
auto stream = std::move(incoming).value();
auto read = co_await stream.read_some(pace::as_result, buffer);
if (!read) {
    log_error(read.error());
} else if (read.value() == 0) {
    // EOF when buffer is nonempty.
}
```

Check the result before calling `value()`, which throws `std::system_error` on failure. Success has an empty
`error()`, including TCP EOF and an empty UDP datagram. A failed `write_all` may have sent a prefix.
A closed socket reports `bad_file_descriptor`. Closing it during a wait reports `operation_canceled`.
Deadlines report `timed_out`. An overlapping TCP read/write/accept or UDP send/receive in the same direction
reports `operation_in_progress`.

Existing tasks, including TLS and QUIC operations, can be awaited as `co_await pace::as_result(task)`.
This form converts exceptions from the underlying task. A `task<result<T>>` keeps the original
`std::system_error` code and category. It maps invalid arguments to `invalid_argument`, length errors to
`message_size`, other logic errors to `operation_not_permitted`, allocation failures in the task body to
`not_enough_memory`, and other exceptions to `io_error`. Task creation and argument evaluation follow ordinary
C++ allocation and exception rules.

## Error codes

A `std::system_error` carries a `std::error_code`. Compare it with `std::errc` values rather than matching the
message:

| Code | Raised when |
|:--|:--|
| `std::errc::operation_canceled` | The context was stopped, a cancellation token was triggered, or the socket was closed while the operation was suspended |
| `std::errc::timed_out` | A deadline expired: a `blocking_options` deadline, a scope deadline, an HTTP deadline, or a whole DNS lookup |
| `std::errc::bad_file_descriptor` | The socket or stream was already closed |
| `std::errc::protocol_error` | A TLS handshake, certificate verification or record-layer failure; the message contains OpenSSL's reason |
| `std::errc::not_supported`, `operation_not_supported`, `address_family_not_supported` | The platform or build lacks the feature, such as an option the OS does not offer |
| `std::errc::message_size`, `no_buffer_space` | A datagram did not fit or the OS could not queue it |
| any other value | Passed through from the operating system (`std::system_category()` or `std::generic_category()`) |

DNS failures keep c-ares status codes in `pace::dns::error_category()`; DNS cancellation and expiry of the whole
lookup use the generic `operation_canceled` and `timed_out` instead.

```cpp
try
{
    co_await stream.read_some(buffer);
}
catch (const std::system_error& error)
{
    if (error.code() == std::errc::operation_canceled)
    {
        co_return; // shutting down: expected
    }
    throw;
}
```

## Threads and concurrency

An exception thrown inside a root task or a posted function, and not caught there, stops its `io_context`;
`run()` rethrows the first such exception after the context has drained. A `runtime` stops its other workers
when one fails and rethrows after joining them. While a context is stopping, an uncaught
`operation_canceled` from a root task is not treated as a failure.

## See also

- [Runtime](runtime/README.md) — how errors and cancellation propagate through tasks and contexts
- [Structured concurrency](runtime/structured-concurrency.md) — cancellation tokens and deadlines
- [Execution model](../explanation/execution-model.md#errors-across-threads)
