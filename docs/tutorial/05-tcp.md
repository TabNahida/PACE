# 5. TCP

[中文](05-tcp.zh.md) · [Learning path](README.md) · [Previous: tasks and timers](04-tasks-and-timers.md)

This chapter builds a TCP client and an echo server. The protocol is four request bytes and four reply bytes, on a
numeric loopback address. Both programs link `pace_net`.

## Write the client first

Save this as `tcp_client.cpp`:

```cpp
#include <pace/net.hpp>
#include <iostream>

int main()
{
    try
    {
        auto reply = pace::tcp::exchange({"127.0.0.1", 9090}, "ping", 4);
        std::cout << reply << '\n';
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`exchange` connects, writes the whole request, reads exactly four bytes and closes the connection. It returns an
owned string and blocks this thread, with a total budget of 30 seconds by default. No `run` or coroutine is needed.
For a different budget, include `<chrono>` and pass `{.timeout = std::chrono::seconds(2)}` as the fourth argument.

TCP delivers a stream of bytes, **not messages**. The last argument is the exact length of the reply, not a maximum
size. PACE keeps reading through short reads and throws if the peer closes before four bytes arrive. Both peers must
agree on how messages are framed.

## Supply the four-byte echo service

Save this as `tcp_server.cpp`:

```cpp
#include <pace/net.hpp>
#include <pace/runtime.hpp>
#include <iostream>

pace::task<void> echo(pace::tcp::stream socket)
{
    try
    {
        auto message = co_await pace::read_exact(socket, 4);
        co_await pace::write(socket, std::move(message));
    }
    catch (const std::system_error& error)
    {
        std::cerr << "connection: " << error.what() << '\n';
    }
}

pace::task<void> serve()
{
    auto listener = pace::tcp::listen({"127.0.0.1", 9090});
    for (;;)
    {
        pace::this_context().spawn(echo(co_await listener.accept()));
    }
}

int main()
{
    try
    {
        pace::run(serve);
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`listen` binds the address on the running context. Each `accept` returns one connected stream, and `spawn` hands it
to an independent `echo` task so the loop can accept the next connection at once. The `echo` coroutine owns its
stream and handles that connection's errors itself. The server uses `run` because it keeps processing events; a
one-shot client like the one above does not need it.

## Build and try both sides

Add these targets to `xmake.lua`:

```lua
for _, name in ipairs({"tcp_client", "tcp_server"}) do
    target(name)
        set_kind("binary")
        add_files(name .. ".cpp")
        add_deps("pace_net")
end
```

Build with `xmake -y tcp_client tcp_server`. Run `xmake run tcp_server` in one terminal, then `xmake run tcp_client`
in another. Expect `ping` and the client to exit. Run the client again: the server is still listening. Stop the
server with Ctrl-C.

Change both lengths to six and the payload to `hello!` for a six-byte exchange. Changing only one side breaks the
protocol: depending on which side you change, the client reports that the connection closed early or waits until
its timeout.

## Compose async work when needed

Inside a running coroutine, the same one-shot exchange is:

```cpp
auto reply = co_await pace::tcp::async_exchange({"127.0.0.1", 9090}, "ping", 4);
```

For several exchanges on one connection, keep the socket yourself:

```cpp
auto socket = co_await pace::tcp::connect({"127.0.0.1", 9090});
co_await pace::write(socket, "ping");
auto reply = co_await pace::read_exact(socket, 4);
co_await socket.close_gracefully();
```

`pace::write` owns its string while it waits. `read_exact` returns an owned string and refuses lengths above 1 MiB
unless you pass a larger maximum. The socket must not outlive its context. The async calls do not add the 30-second
budget of the blocking call.

The companion `xmake run tutorial_tcp` ([source](../../examples/tutorial_tcp.cpp)) runs a full echo server and a
client in one process and prints `TCP echo verified: Hello from PACE`. Its echo loop reads into a reusable buffer
with `read_some` and writes back what arrived:

```cpp
std::array<char, 4096> buffer{};
while (const auto count = co_await socket.read_some(pace::as_writable_bytes(buffer)))
{
    co_await socket.write_all(pace::as_bytes(std::string_view(buffer.data(), count)));
}
```

Streams read and write bytes (`std::span<std::byte>`). `pace::as_writable_bytes` lets `read_some` fill a `char`
buffer, and `pace::as_bytes` hands text to `write_all`; neither copies. `read_some` returns how many bytes arrived,
possibly fewer than the buffer holds, and `0` only when the peer has closed its side. Unlike `pace::write`,
`write_all` borrows its bytes, so the buffer must not change until the write completes. The accept loop treats a
closed listener as the end of the loop. `xmake run tutorial_tcp serve` keeps the server on port 9090 until Ctrl-C;
`xmake run tutorial_tcp client` talks to it.

[TCP reference](../reference/net/tcp.md) · [Protocol tuning](../guides/protocol-tuning.md) ·
[Orderly shutdown](09-graceful-shutdown.md)

Next: [6. UDP](06-udp.md)
