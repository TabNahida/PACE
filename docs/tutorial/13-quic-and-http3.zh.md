<!-- source-hash: e50bf8091e2e -->
# 13. QUIC 与 HTTP/3

[English](13-quic-and-http3.md) · [学习路线](README.zh.md) · [上一章：HTTP/2](12-http2.zh.md)

QUIC 在 UDP 之上提供经过认证的可靠流。HTTP/3 再在这些流上加入 HTTP 分帧和 QPACK 头部压缩。请一层一层地构建：原始 UDP 数据报不是 QUIC，QUIC 流上的任意字节也不是 HTTP/3 消息。

这两个模块都是可选的，且需要 POSIX 平台。`--http3=y` 会同时启用 QUIC；xmake 第一次会下载 OpenSSL、ngtcp2 和 nghttp3。`--quic_backend=ngtcp2`（默认）或 `--quic_backend=openssl` 选择 QUIC 实现；本章内容在两者上都适用。

## 从一个 HTTP/3 URL 开始

以 `--http3=y` 配置并链接 `pace_http3`。对于证书能被 OpenSSL 默认信任库接受的 HTTP/3 服务：

```cpp
#include <pace/http3.hpp>
#include <iostream>

int main()
{
    auto reply = pace::http3::get("https://127.0.0.1:9443/health");
    std::cout << reply.status << " " << reply.body;
}
```

HTTP/3 URL 总是 `https://`。对于使用测试证书的服务器，创建一个 `quic::context`，加载测试 CA，设置 ALPN `h3`，然后调用 `pace::http3::get(trust, url)`。在协程中使用 `co_await pace::http3::async_get(url)`。信任和 URL 选项见 [URL 客户端](../reference/http/clients.zh.md)。本章其余部分构建一个本地服务，并展示其下的 QUIC 流；URL 请求本身并不需要这些设置。

## 配置身份和应用协议

以 `--quic=y` 配置，链接 `pace_quic` 并包含 `<pace/quic.hpp>`。用[第 10 章](10-tls-and-dns.zh.md)的测试证书、私钥和 CA 创建服务端与客户端的 `quic::context`，并在两端设置 ALPN `pace-demo`：

```cpp
pace::quic::context server_config(pace::quic::role::server), client_config;
server_config.use_certificate_chain("tests/certs/localhost.pem", "tests/certs/localhost-key.pem");
client_config.load_verify_file("tests/certs/ca.pem");
server_config.set_alpn_protocols({"pace-demo"});
client_config.set_alpn_protocols({"pace-demo"});
auto listener = pace::quic::listen(io, {"127.0.0.1", 0}, server_config);
```

把这些对象放在对端任务之外，让它们存活到 `io.run()` 返回。路径相对于 PACE 源码目录，这些凭据是公开的测试凭据。QUIC 自带 TLS 1.3 握手；不需要用于 TCP 的独立 TLS 模块。

## 打开一个流，并显式结束一个方向

在客户端协程中：

```cpp
auto connection = co_await pace::quic::connect(client_config, endpoint, "localhost");
auto stream = co_await connection.open_stream();
co_await pace::write(stream, "hello");
stream.finish_write();
```

在服务端，先等待 `listener.accept()`，再等待 `connection.accept_stream()`。一直读到该流结束，写出回复，再为回复方向调用 `finish_write()`。流结束标记（FIN）只作用于这一个流，而不是连接上的其他流。在流使用期间保持连接存活。`pace::write` 完成并不能证明对端已收到这些字节；关闭发送方之前，请遵循[协议调优](../guides/protocol-tuning.zh.md)中关于送达和 FIN 的规则，下面的配套程序就是这样做的。

配套程序 [`tutorial_quic.cpp`](../../examples/tutorial_quic.cpp) 会等到服务器的回复被确认、两端都结束后才停止，并在五秒后判定失败。以 `--quic=y --examples=y` 配置 PACE 源码目录，运行 `xmake run tutorial_quic`，预期输出 `Hello over verified QUIC!`。在你自己的副本中试试两个流，确认第一个流上的 FIN 不会关闭第二个流。

## 让 HTTP/3 接管协议流

以 `--http3=y` 配置，链接 `pace_http3`，并把两端的 ALPN 列表都改为 `h3`。[第 3 章](03-http-server.zh.md)的请求、响应和路由器类型依然适用：

```cpp
pace::http::router app;
app.get("/health", [](pace::http::request) { return pace::http::response{.body = "ok"}; });
io.spawn(pace::http3::serve(listener, app));
```

包含 `<pace/http3.hpp>` 和 `<pace/http.hpp>`。在客户端，用 `h3` 配置打开一个经过校验的 QUIC 连接，然后请求一个响应：

```cpp
auto connection = co_await pace::quic::connect(client_config, endpoint, "localhost");
const auto reply = co_await pace::http3::async_get(std::move(connection), "localhost", "/health");
std::cout << reply.status << " " << reply.body << '\n';
```

`async_get` 会接管这个连接：不要在它上面打开你自己的流。HTTP/3 使用它自己的控制流和 QPACK 流。`async_get` 返回一个缓冲好的响应；要在一个连接上发多个请求，使用 `async_exchange`。[协议调优](../guides/protocol-tuning.zh.md)介绍批量请求、并发和生命周期。这个调用运行在你已有的上下文上，不会启动后台线程。

## 验证分层后的版本

在 PACE 源码目录中：

```sh
xmake f -m release --http3=y --examples=y -y
xmake -y tutorial_quic tutorial_http3
xmake run tutorial_quic
xmake run tutorial_http3
```

配套程序 [`tutorial_http3.cpp`](../../examples/tutorial_http3.cpp) 先用 `http3::async_get` 发送一个 GET，再用 `http3::async_exchange` 把 `/one` 和 `/two` 作为一批发送，输出 `Hello over HTTP/3, /one!` 和 `Hello over HTTP/3, /two!`。在你的副本中，改掉一端的 ALPN，确认连接失败：能访问 UDP 端口并不等于建立了经过认证的 HTTP/3 会话。这个 API 不支持服务器推送、CONNECT、WebTransport 和 0-RTT。限制和所有权规则见 [QUIC](../reference/quic/README.zh.md) 和 [HTTP/3](../reference/http3/README.zh.md) 参考。

## 编写一个完整的单请求服务

按照[第 5 章](05-tcp.zh.md)的方式建立 `h3_server` 和 `h3_client` 目标，两者都依赖 `pace_http3`。测试证书路径相对于 PACE 源码目录，所以要像[第 10 章](10-tls-and-dns.zh.md)那样，在 `hello` 目录下用 `xmake run -w third_party/PACE` 启动这两个程序。先写服务器：

```cpp
#include <pace/http.hpp>
#include <pace/http3.hpp>
#include <pace/runtime.hpp>
#include <iostream>

pace::task<void> serve()
{
    pace::quic::context security(pace::quic::role::server);
    security.use_certificate_chain("tests/certs/localhost.pem", "tests/certs/localhost-key.pem");
    security.set_alpn_protocols({"h3"});
    auto listener = pace::quic::listen({"127.0.0.1", 9094}, security);
    pace::http::router routes;
    routes.get("/health", [] { return pace::http::text("ok\n"); });
    co_await pace::http3::serve_connection(co_await listener.accept(), routes);
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

监听器在正在运行的上下文上绑定，并在整个连接期间保持存活。然后是客户端；虽然地址是数字形式，它仍以 `localhost` 作为证书身份：

```cpp
#include <pace/http3.hpp>
#include <pace/runtime.hpp>
#include <iostream>

pace::task<pace::http::response> get_health()
{
    pace::quic::context trust;
    trust.load_verify_file("tests/certs/ca.pem");
    trust.set_alpn_protocols({"h3"});
    auto connection = co_await pace::quic::connect(trust, {"127.0.0.1", 9094}, "localhost");
    co_return co_await pace::http3::async_get(std::move(connection), "localhost:9094", "/health");
}

int main()
{
    try
    {
        auto reply = pace::run(get_health);
        std::cout << reply.status << " " << reply.body;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

先运行 `xmake run -w third_party/PACE h3_server`，再在另一个终端运行 `xmake run -w third_party/PACE h3_client`。预期输出 `200 ok`，两个程序随后退出。下一次请求之前要重新启动服务器。要在循环中接受连接，把 `serve_connection(co_await listener.accept(), routes)` 换成 `serve(listener, routes)`。测试证书仅用于这个本地练习。

在 QUIC 配套程序中，客户端读到回复的 FIN 之后仍保持存活，直到服务器看到它的确认：它会一直等待连接事件，直到两端都结束。收到 FIN 并不意味着远端发送方已经完成。

下一章：[14. 静态文件与反向代理](14-gateway.zh.md)
