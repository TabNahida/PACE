<!-- source-hash: b2b87554fdc2 -->
# 12. HTTP/2

[English](12-http2.md) · [学习路线](README.zh.md) · [上一章：WebSocket](11-websocket.zh.md)

HTTP/2 是可选模块：用 `--http2=y` 配置 `hello` 项目（xmake 第一次会下载 nghttp2），并链接 `pace_http2`。按照[第 5 章](05-tcp.zh.md) TCP 那对程序的方式，建立 `h2_server` 和 `h2_client` 两个目标。本章从基于先验知识的明文 HTTP/2 开始：没有 HTTP/1 升级步骤。

## 路由不变，协议更换

保存为 `h2_server.cpp`：

```cpp
#include <pace/http.hpp>
#include <pace/http2.hpp>
#include <pace/runtime.hpp>
#include <iostream>

pace::task<void> serve()
{
    auto listener = pace::tcp::listen({"127.0.0.1", 9093});
    pace::http::router routes;
    routes.get("/health", [] { return pace::http::text("ok\n"); });
    co_await pace::http2::serve_connection(co_await listener.accept(), routes);
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

路由器和响应辅助函数与[第 3 章](03-http-server.zh.md)中 `http::app` 为 HTTP/1 使用的相同。`serve_connection` 接管一个已接受的连接，在其上提供 HTTP/2 服务，直到对端断开。这个练习在该连接结束后退出；`http2::serve(listener, routes)` 则在循环中接受连接。

## 直接请求一个响应

保存为 `h2_client.cpp`：

```cpp
#include <pace/http2.hpp>
#include <iostream>

int main()
{
    try
    {
        auto reply = pace::http2::get("http://127.0.0.1:9093/health");
        std::cout << reply.status << " " << reply.body;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`http2::get(url)` 直接返回响应，默认预算与 HTTP/1 调用一样是 30 秒。在协程中写作 `co_await pace::http2::async_get(url)`。主机名需要 `--dns=y`；`https://` URL 需要 `--tls=y`。见 [URL 客户端](../reference/http/clients.zh.md)。

## 先验证，再增加请求

构建两个目标。启动 `xmake run h2_server`，再运行 `xmake run h2_client`，预期输出 `200 ok`。下一次运行客户端之前要重新启动服务器。请求另一个路径，看看 404 响应。

配套程序 [`tutorial_http2.cpp`](../../examples/tutorial_http2.cpp) 用 `http2::serve` 提供一个协程处理函数，先在自己打开的连接上用 `http2::async_get(stream, authority, target)` 发送一个 GET，再在第二个连接上用 `http2::async_exchange` 把 `/one` 和 `/two` 作为一批发送。`xmake run tutorial_http2`（以 `--http2=y --examples=y` 配置）输出 `Hello, /one!` 和 `Hello, /two!`。

`async_exchange` 让多个请求共享一个连接：这一批请求一起发出，响应按请求顺序返回。循环发送单个 GET 会为每个请求各开一个连接，失去多路复用的好处。要使用经过校验的 TLS，用[第 10 章](10-tls-and-dns.zh.md)的 context 在两端设置 ALPN `h2`，再把握手完成的 TLS 流交给 `async_get` 或 `async_exchange`。[协议调优](../guides/protocol-tuning.zh.md)介绍批量请求和处理函数的限制；[HTTP/2 参考](../reference/http2/README.zh.md)介绍 TLS 服务器重载和流量控制。

下一章：[13. QUIC 与 HTTP/3](13-quic-and-http3.zh.md)
