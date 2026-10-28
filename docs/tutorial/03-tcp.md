# 3. Send a TCP request with one call

[中文](03-tcp.zh-CN.md) · [Learning path](README.md) · [Next: HTTP](04-http.md)

Use PACE 0.4.0 or newer and chapter 1's source setup; link `pace_net`. Our protocol is four request bytes and four
reply bytes, using a numeric loopback address.

## 1. Write the client first

Save this as `tcp_client.cpp`:

```cpp
#include <pace/tcp.hpp>
#include <iostream>

int main() {
    try {
        auto reply = pace::tcp::exchange({"127.0.0.1", 9090}, "ping", 4);
        std::cout << reply << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`exchange` connects, writes the entire request, reads exactly four bytes and closes
the connection. It returns an owned string and blocks this thread, with a 30-second
default total deadline. No `run` or coroutine is needed. For a different budget,
include `<chrono>` and pass `{.timeout = std::chrono::seconds(2)}` as the fourth argument.

TCP preserves bytes, **not message boundaries**. The last argument here is the
expected reply length, not a maximum packet size. PACE handles short reads and
throws on premature EOF. Both peers must agree on framing.

## 2. Supply the four-byte echo service

Save this as `tcp_server.cpp`:

```cpp
#include <pace/run.hpp>
#include <pace/tcp.hpp>
#include <iostream>

pace::task<void> echo(pace::tcp::stream socket) {
    try {
        auto message = co_await pace::read_exact(socket, 4);
        co_await pace::write(socket, std::move(message));
    } catch (const std::system_error& error) {
        std::cerr << "connection: " << error.what() << '\n';
    }
}
pace::task<void> serve() {
    auto listener = pace::tcp::listen({"127.0.0.1", 9090});
    for (;;) pace::this_context().spawn(echo(co_await listener.accept()));
}
int main() {
    try { pace::run(serve); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`listen` binds on the running context. Each `accept` returns one connected stream;
`spawn` gives it to an independent `echo` task so the listener can accept again.
The coroutine owns its stream and handles connection errors. Servers use `run`
because they keep processing events; ordinary one-shot clients do not need it.

## 3. Build and try both sides

Add these targets to chapter 1's xmake.lua:

```lua
for _, name in ipairs({"tcp_client", "tcp_server"}) do
    target(name)
        set_kind("binary")
        add_files(name .. ".cpp")
        add_deps("pace_net")
end
```

Build with `xmake -y tcp_client tcp_server`. Run `xmake run tcp_server` in one
terminal, then `xmake run tcp_client` in another. Expect `ping` and client exit.
Run the client again; the server is still listening. Stop the server with Ctrl-C.
Try a six-byte request/reply by changing both lengths and the payload to `hello!`.
Changing only one side breaks the protocol.

## 4. Compose async work when needed

Inside a running coroutine, the equivalent one-shot operation is:

```cpp
auto reply = co_await pace::tcp::async_exchange({"127.0.0.1", 9090}, "ping", 4);
```

For multiple exchanges on a connection, keep the socket yourself:

```cpp
auto socket = co_await pace::tcp::connect({"127.0.0.1", 9090});
co_await pace::write(socket, "ping");
auto reply = co_await pace::read_exact(socket, 4);
co_await socket.close_gracefully();
```

`pace::write` owns its string across suspension. `read_exact` returns an owned
string, bounded to 1 MiB by default. The socket must stay inside its context's
lifetime. The async helper does not add the blocking helper's total deadline.

[Direct-client bounds and errors](../reference/direct-clients.md) ·
[Buffers, IPv6 and concurrency](../reference/component-conveniences.md) ·
[Tuning](../reference/protocol-tuning.md) · [Shutdown](06-shutdown.md)

The repository's `xmake run tutorial_tcp` is a self-contained asynchronous echo exercise.
