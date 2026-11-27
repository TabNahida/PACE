<!-- source-hash: 6b0313141bd2 -->
# 2. 发送 HTTP 请求

[English](02-http-client.md) · [学习路线](README.zh.md) · [上一章：第一个程序](01-install-and-first-program.zh.md)

发一个 HTTP 请求不需要协程、事件循环或 `run`：直接客户端会阻塞调用线程并返回响应。学习本章时，请在一个终端里保持[第 1 章](01-install-and-first-program.zh.md)的 `hello` 服务器运行。

## 编写客户端

在 `hello` 项目中保存为 `client.cpp`：

```cpp
#include <pace/http1.hpp>
#include <iostream>

int main()
{
    try
    {
        auto reply = pace::http::get("http://127.0.0.1:8080/health");
        std::cout << reply.status << " " << reply.body;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

在 `xmake.lua` 中为它添加一个目标：

```lua
target("client")
    set_kind("binary")
    add_files("client.cpp")
    add_deps("pace_http1")
```

用 `xmake -y client` 构建，再运行 `xmake run client`。预期输出 `200 ok`。

`get` 会建立连接、发送请求、读取完整响应并关闭连接，然后才返回。它阻塞当前线程，`reply` 拥有自己的状态码、头部和 body。404 这样的 HTTP 错误状态是一个响应，而不是异常：请检查 `reply.status`。连接错误和超时抛出 `std::system_error`，格式错误的响应抛出 `http::protocol_error`。客户端不会跟随重定向，也不会重试请求。

## 发送 body

把 `get` 那一行换成：

```cpp
auto reply = pace::http::post("http://127.0.0.1:8080/echo", "hello", "text/plain");
```

重新构建并运行客户端，预期输出 `200 hello`。第三个参数是 `Content-Type`；省略时 body 以 `application/octet-stream` 发送。

## 选择超时

直接调用默认有 30 秒的总预算，涵盖建立连接、发送请求和接收响应。要让一次 GET 只用两秒，包含 `<chrono>` 并在 URL 后传入选项：

```cpp
auto reply = pace::http::get("http://127.0.0.1:8080/health", {.timeout = std::chrono::seconds(2)});
```

预算用尽时，调用抛出 `std::system_error`，错误码为 `std::errc::timed_out`。停掉服务器再运行客户端，可以看到连接被拒绝时的报错。

## 使用其他方法

`fetch` 发送你自己构造的请求，例如一个 HEAD 请求：

```cpp
pace::http::request request;
request.method = "HEAD";
auto reply = pace::http::fetch("http://127.0.0.1:8080/health", std::move(request));
```

预期输出 `200 `，body 为空。URL 提供请求目标和 `Host` 头部；你自己添加的 `Host` 头部会被拒绝。请求需要的其他内容通过 `request.headers` 和 `request.body` 设置。

## 访问主机名和 HTTPS

前面的 URL 都使用数字地址，默认构建即可支持。主机名 URL 需要 DNS，`https://` URL 需要 TLS。启用两者后重新配置；xmake 第一次会下载 c-ares 和 OpenSSL：

```sh
xmake f -m release --tests=n --examples=n --dns=y --tls=y -y
```

然后同样的调用就能访问公开的 HTTPS 服务器：

```cpp
auto reply = pace::http::get("https://example.com/");
```

HTTPS 用 OpenSSL 的默认信任库校验服务器证书，并检查证书是否与 URL 中的主机匹配。如果这个信任库为空，校验会以 `unable to get local issuer certificate` 失败；在 macOS 上使用 xmake 构建的 OpenSSL 时就是这种情况。此时可以把环境变量 `SSL_CERT_FILE` 指向一个 CA 证书包（例如 `/etc/ssl/cert.pem`），或者自己加载证书包。要信任你自己的 CA，在 URL 之前传入一个 `tls::context`：`pace::http::get(trust, url)`。[第 10 章](10-tls-and-dns.zh.md)会构造这样的 context 和一个本地 TLS 对端。

## 一次调用完成 TCP 和 UDP

TCP 和 UDP 也有同类的直接调用。配合[第 5 章](05-tcp.zh.md)的 TCP 回显服务器：

```cpp
auto reply = pace::tcp::exchange({"127.0.0.1", 9090}, "ping", 4);
```

配合[第 6 章](06-udp.zh.md)的 UDP 回显服务器：

```cpp
auto packet = pace::udp::exchange({"127.0.0.1", 9091}, "hello");
if (packet.truncated)
{
    throw std::runtime_error("UDP reply truncated");
}
```

包含 `<pace/net.hpp>` 并链接 `pace_net`。两者都接受与 HTTP 调用相同的超时选项。TCP 需要给出回复的长度，因为 TCP 不保留消息边界；UDP 返回一个数据报及其发送方。

## 组合多项工作时改用异步

在 PACE 协程内部，使用 `async_get`、`async_post` 或 `async_fetch`：

```cpp
pace::run(
    []() -> pace::task<void>
    {
        auto reply = co_await pace::http::async_get("http://127.0.0.1:8080/health");
        std::cout << reply.body;
    }
);
```

`pace::run` 在当前线程上创建事件循环，并把协程运行到结束。请求等待期间，事件循环可以处理其他工作。当程序本身已经在运行任务、处理连接或同时发出多个请求时，就该使用异步形式。阻塞调用在正在运行的 PACE 上下文中被调用时会抛出 `std::logic_error`，所以处理函数和协程里一律使用异步形式。异步调用不附加 30 秒预算；需要时请自行设置截止时间（[第 7 章](07-structured-concurrency.zh.md)）。

配套程序 `xmake run tutorial_http_client` 在本地监听器上提供一个协程处理函数，并用 `async_get` 检查它的分块响应。[第 4 章](04-tasks-and-timers.zh.md)讲解任务和 `pace::run`。[HTTP/1](../reference/http1/README.zh.md) 和 [URL 客户端](../reference/http/clients.zh.md)列出了所有重载和默认值。

下一章：[3. 构建 HTTP 服务器](03-http-server.zh.md)
