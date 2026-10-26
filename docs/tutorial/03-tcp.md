# 3. Build a TCP echo client and server

[中文](03-tcp.zh-CN.md) · [Previous: tasks](02-tasks-timers.md) · [Next: HTTP](04-http.md)

We will build a server that sends back every byte it receives, then a client that
verifies one complete message. Use chapter 1's project and link `pace_net`.
TCP endpoints contain numeric IP addresses; hostname lookup comes in chapter 8.

## Give each connection its own task

Write `tcp_server.cpp` with the following complete program:

```cpp
#include <pace/tcp.hpp>
#include <array>
#include <iostream>

pace::task<void> echo(pace::tcp::stream socket) {
    try {
        std::array<char, 4096> buffer{};
        while (const auto count = co_await socket.read_some(buffer))
            co_await socket.write_all(std::string_view(buffer.data(), count));
    } catch (const std::system_error& error) {
        std::cerr << "connection: " << error.what() << '\n';
    }
}

pace::task<void> accept_clients(pace::tcp::listener& listener) {
    for (;;) listener.context().spawn(echo(co_await listener.accept()));
}

int main() {
    try {
        pace::io_context io;
        pace::tcp::listener listener(io, {"127.0.0.1", 9090});
        io.spawn(accept_clients(listener));
        io.run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`listener` binds during construction. `accept` suspends until a connection arrives;
`spawn` transfers the accepted stream into a separate connection task, so the
accept loop can wait for the next client. The stream is passed **by value** and
is closed by its destructor. The listener is outside the tasks and stays alive
through `io.run()`.

A read returns a byte count, not a C string. A zero result with a nonempty buffer
means EOF. `write_all` completes before the array is reused, so its borrowed view
stays valid. A connection failure is handled inside `echo`; it need not stop the
accept loop. This first server uses the OS's default Ctrl-C termination. Chapter 6
adds cooperative cancellation and explains why closing a listener needs an accept
error path.

## Write a client with a clear completion point

Put this in `tcp_client.cpp`:

```cpp
#include <pace/run.hpp>
#include <pace/tcp.hpp>
#include <iostream>
#include <span>
#include <string>

pace::task<std::string> exchange(pace::io_context& io) {
    pace::tcp::stream socket(io);
    co_await socket.connect({"127.0.0.1", 9090});
    const std::string message = "Hello from PACE";
    co_await socket.write_all(message);
    std::string reply(message.size(), '\0');
    std::size_t received = 0;
    while (received != reply.size()) {
        const auto n = co_await socket.read_some(
            std::span<char>(reply.data() + received, reply.size() - received));
        if (n == 0) throw std::runtime_error("EOF before the complete reply");
        received += n;
    }
    if (reply != message) throw std::runtime_error("echo mismatch");
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

Why the accumulation loop? TCP preserves byte order, **not message boundaries**.
One write may require several reads. Here the client already knows the expected
length. A real protocol must carry a length prefix, a delimiter or another framing
rule. Changing this loop to a single read is incorrect even if it works locally.

`close_gracefully` half-closes outgoing bytes and performs a bounded peer drain;
do it before entry completion stops the context. It is not proof that an
application on the other side processed your message.

## Build and observe both sides

Add two targets to your application's xmake file:

```lua
for _, name in ipairs({"tcp_server", "tcp_client"}) do
    target(name)
        set_kind("binary")
        add_files(name .. ".cpp")
        add_deps("pace_net")
end
```

Run `xmake -y tcp_server tcp_client`. In one terminal run `xmake run tcp_server`;
in another run `xmake run tcp_client`. Expect `Hello from PACE`. Run the client a
second time to see that the server is still listening. Stop it with Ctrl-C.
The repository's `xmake run tutorial_tcp` runs both sides on an ephemeral port and
verifies the same behavior without manual terminals.

## Change one property at a time

Use `::1` on **both** sides for IPv6 loopback. Use port zero with
`listener.local_endpoint()` for a test-selected port. Bind `0.0.0.0` or `::` only
when you want external interfaces; IPv6 listeners are IPv6-only unless explicitly
configured otherwise. A stream supports one read and one write at a time, not two
simultaneous reads or writes. Objects and their buffers must outlive active member
operations; closing a socket is not permission to destroy it before cancellation
has unwound. [Chapter 6](06-shutdown.md) explains that order.

[Complete API](../reference/tcp.md) · [Companion source](../../examples/tutorial_tcp.cpp)
