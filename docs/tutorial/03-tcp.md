# 3. TCP echo: client and server

Complete source: [`tutorial_tcp.cpp`](../../examples/tutorial_tcp.cpp).
Its default mode starts a loopback listener on an OS-selected port, runs one
client, verifies every echoed byte and stops:

```sh
xmake run tutorial_tcp
```

Expected output is `TCP echo verified: Hello from PACE`. This is a functional
smoke check, not a performance benchmark.

For interactive use, start the server in one terminal and the client in another:

```sh
xmake run tutorial_tcp serve 9090
xmake run tutorial_tcp client 9090
```

Use Ctrl-C to request server shutdown. The signal handler only sets an atomic
flag; an ordinary coroutine observes it and calls `stop()`. The listener binds
loopback so the example is not exposed on every interface.

## Accept an owned connection

Construct the listener before running the context:

```cpp
pace::io_context io;
pace::tcp::listener listener(io, {"127.0.0.1", 9090});
io.spawn(accept_connections(listener));
io.run();
```

The listener is destroyed before `io`, after run has drained. The named accept
task borrows the listener; each accepted stream is moved into an owning echo task:

```cpp
for (;;) listener.context().spawn(echo(co_await listener.accept()));
```

The full source catches expected cancellation/closed-listener errors around this
loop. Accepted streams belong to the listener's context. Passing a stream to
another context does not rebind its I/O ownership.

The connection handler owns both socket and buffer for their complete lifetimes:

```cpp
pace::task<void> echo(pace::tcp::stream socket) {
    try {
        socket.set_no_delay();
        std::array<char, 4096> buffer{};
        while (const auto count = co_await socket.read_some(buffer))
            co_await socket.write_all(std::string_view(buffer.data(), count));
    } catch (const std::system_error&) {
        // Treat network failures as per-connection events.
    }
}
```

The view passed to `write_all` remains valid because the array is in this coroutine
frame and is not modified until the write finishes. A nonempty-buffer read of zero
means EOF. Destruction then closes the stream. Production code can log selected
per-client errors here without failing the entire reactor.

## Connect and frame messages

The client takes its endpoint and message by value, creates a stream on `io`, and
awaits `connect`. `write_all(message)` retains a view into that owned message while
the write is active. TCP is a byte stream: one write is not guaranteed to match one
read. The example accumulates reads until exactly `message.size()` bytes arrive,
failing on early EOF or a mismatched payload.

Your protocol needs a length prefix, delimiter or another framing rule. Never
assume a single `read_some` fills the buffer. Embedded zero bytes are valid;
use returned lengths rather than C string functions. You may have one read and
one write concurrently, but not two reads or two writes on the same stream.

After verification the client awaits `close_gracefully()`: it half-closes sends,
waits briefly for peer EOF while draining, and closes the handle. There must be
no other operations active on that stream. It then stops its own context.

## Common adjustments

- Pass `{"0.0.0.0", port}` to listen on all interfaces when that is intended.
- Pass port `0` and use `listener.local_endpoint().port` for an ephemeral test port.
- Keep hostname resolution outside the I/O loop; endpoints currently accept only numeric IPv4.
- Catch connect/read/write failures in the connection task. No per-operation timeout is built in.
- For same-port multiworker servers, use separate listeners with supported `reuse_port`; see [execution](05-execution.md).

Reference: [`pace::tcp`](../reference/tcp.md).
Next: [HTTP services](04-http.md).
