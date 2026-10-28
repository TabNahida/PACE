# 3. Connect, write and read a complete TCP message

[中文](03-tcp.zh-CN.md) · [Previous: tasks](02-tasks-timers.md) · [Next: HTTP](04-http.md)

Use chapter 1's **current source checkout** and link `pace_net`. We will send four
bytes, `ping`, and receive exactly four back. Numeric addresses keep this first
exercise independent of DNS.

## 1. Listen and accept

Save this complete program as `tcp_server.cpp`:

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

`listen` binds immediately on the running context. `accept` waits for a connection;
`spawn` gives its stream to a separate `echo` task, so another client can connect.
`echo` owns the socket and catches connection failures. The listener remains in
`serve` while it awaits clients. `run` keeps the event loop running until this
entry completes; this server runs continuously, so stop it with Ctrl-C.

## 2. Connect and exchange a message

Save this complete program as `tcp_client.cpp`:

```cpp
#include <pace/run.hpp>
#include <pace/tcp.hpp>
#include <iostream>

pace::task<std::string> exchange() {
    auto socket = co_await pace::tcp::connect({"127.0.0.1", 9090});
    co_await pace::write(socket, "ping");
    auto reply = co_await pace::read_exact(socket, 4);
    if (reply != "ping") throw std::runtime_error("echo mismatch");
    co_await socket.close_gracefully();
    co_return reply;
}
int main() {
    try { std::cout << pace::run(exchange) << '\n'; }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`connect` returns a connected socket. `pace::write` owns the supplied string until
it has been written; `read_exact` returns an owned string of the requested length.
TCP preserves bytes, **not message boundaries**: a single read need not return
all four bytes. `read_exact` accumulates them and throws if EOF arrives too early.
For this exercise both sides agree on four bytes; a general protocol needs an
explicit framing rule, such as a length prefix.

Finish graceful closing before returning from the entry. Return ordinary data
such as the reply, not sockets that would outlive the context owned by `run`.

## 3. Build both programs

Add these targets to chapter 1's xmake.lua:

```lua
for _, name in ipairs({"tcp_server", "tcp_client"}) do
    target(name)
        set_kind("binary")
        add_files(name .. ".cpp")
        add_deps("pace_net")
end
```

Run `xmake -y tcp_server tcp_client`, then `xmake run tcp_server`. In another
terminal run `xmake run tcp_client`; expect `ping` and process exit. Run the client
again: the server still accepts connections. Try changing both lengths and the
payload to six-byte `hello!`. Changing only one side violates your framing rule.

The [companion](../../examples/tutorial_tcp.cpp), `xmake run tutorial_tcp`, checks
a variable-size echo on an ephemeral port. For reusable buffers, IPv6, no-delay,
allocation bounds and concurrent operation rules, use the
[component reference](../reference/component-conveniences.md) and
[tuning guide](../reference/protocol-tuning.md). [Chapter 6](06-shutdown.md) adds
cooperative shutdown to long-running servers.
