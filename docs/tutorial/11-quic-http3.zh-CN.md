# 11. 从 QUIC stream 走到 HTTP/3

[English](11-quic-http3.md) · [上一章：HTTP/2](10-http2.zh-CN.md) · [学习路线](README.zh-CN.md)

QUIC 在 UDP 上提供经过认证的可靠多路流；HTTP/3 再增加 HTTP 分帧和 QPACK。
先完成 QUIC 流，再交给 HTTP/3 适配层，不要把裸 UDP 数据报当成 QUIC，
也不要把任意 QUIC stream 字节当成 HTTP/3。当前后端需要 POSIX 平台。


## 先用一行 HTTP/3 URL 请求

开启 http3=y，链接 pace_http3。面对可信证书的 HTTP/3 服务：

```cpp
#include <pace/http3.hpp>
#include <iostream>
int main() {
    auto reply = pace::http3::get("https://127.0.0.1:9443/health");
    std::cout << reply.status << " " << reply.body;
}
```

本地测试证书需创建 quic::context trust，加载测试 CA，设置 h3 ALPN，调用
`http3::get(trust, url)`。协程中使用 `co_await http3::async_get(url)`。
选项见[统一客户端](../reference/http-clients.zh-CN.md)。下面建立本地服务，再解释底层 QUIC 流；
只发 URL 请求不需要手动创建流。

## 第一步：先配置身份和应用协议

启用 `--quic=y`，依赖 `pace_quic`，包含 `<pace/quic.hpp>`。
和 TLS 练习一样准备证书、私钥和信任，但使用 quic::context：

```cpp
pace::quic::context server_config(pace::quic::role::server), client_config;
server_config.use_certificate_chain("tests/certs/localhost.pem", "tests/certs/localhost-key.pem");
client_config.load_verify_file("tests/certs/ca.pem");
server_config.set_alpn_protocols({"pace-demo"});
client_config.set_alpn_protocols({"pace-demo"});
auto listener = pace::quic::listen(io, {"127.0.0.1", 0}, server_config);
```

把配置和 listener 放在任务外，保持到 io.run 返回。上述相对路径以仓库根为准，
证书仅用于本地练习。QUIC 自己完成 TLS 1.3 握手，不要求启用单独的 TCP TLS 模块。

## 第二步：打开一个流，明确结束发送方向

客户端协程中写：

```cpp
auto connection = co_await pace::quic::connect(client_config, endpoint, "localhost");
auto stream = co_await connection.open_stream();
co_await pace::write(stream, "hello");
stream.finish_write();
```

服务端先 await listener.accept，再 await connection.accept_stream。
读取到这个 stream 的 EOF 后写回复，并结束回复的发送方向。
FIN 针对一个 stream，不是关闭连接上的全部流。连接对象要活得比流的操作久。

`pace::write` 完成不等于对端已经确认收到；关闭发送方前，应遵守
[送达与 FIN 约定](../reference/protocol-tuning.zh-CN.md#quic)，下方配套程序也演示了这一点。
[QUIC 配套程序](../../examples/tutorial_quic.cpp)等两端完成后才 stop，并有本地期限。
运行 tutorial_quic 应输出 `Hello over verified QUIC!`；之后可尝试增加第二个流，
观察第一个流结束不会自动结束第二个。

## 第三步：让 HTTP/3 接管协议流

启用 `--http3=y`，依赖 `pace_http3`；两端 ALPN 改成 `h3`。
包含 `<pace/http3.hpp>` 和 `<pace/http_router.hpp>`，保留前面章节的路由：

```cpp
pace::http::router app;
app.get("/health", [](pace::http::request) { return pace::http::response{.body = "ok"}; });
io.spawn(pace::http3::serve(listener, app));
```

客户端用 h3 配置完成验证连接，再直接取得一个响应：

```cpp
auto connection = co_await pace::quic::connect(client_config, endpoint, "localhost");
const auto reply = co_await pace::http3::async_get(std::move(connection), "localhost", "/health");
std::cout << reply.status << " " << reply.body << '\n';
```

连接转移后，不再自行打开业务或控制流；HTTP/3 适配层持有控制流和 QPACK 状态。
get 返回一个完整缓冲的响应。多请求时再改用 exchange；
[批处理、并发和生命周期](../reference/protocol-tuning.zh-CN.md#http2-与-http3)放在参考页。
helper 使用已有 context，不创建后台线程。这些新增 helper 需要 PACE v0.3.1 或更新版本。

## 第四步：运行并验证协议协商

在仓库根执行：

```sh
xmake f --http3=y --examples=y -y
xmake -y tutorial_quic tutorial_http3
xmake run tutorial_quic
xmake run tutorial_http3
```

[HTTP/3 配套程序](../../examples/tutorial_http3.cpp)打印 `/one`、`/two` 两个响应。
在副本里把一端 ALPN 改错，确认握手失败：UDP 地址能访问并不代表已建立认证 HTTP/3 会话。
当前没有公开 server push、CONNECT、WebTransport 或 0-RTT 接口。
边界与资源限制见 [QUIC 参考](../reference/quic.md) 和 [HTTP/3 参考](../reference/http3.md)。

## 第五步：写出完整的单请求服务

沿用第三章的两个 target，创建 h3_server.cpp 和 h3_client.cpp，均依赖 pace_http3，
启用 --http3=y。从 PACE checkout 根目录启动进程，使公开测试证书的相对路径有效；
也可以把 tests/certs 复制到自己的练习工作目录。先写服务端：

```cpp
#include <pace/http3.hpp>
#include <pace/http_router.hpp>
#include <pace/run.hpp>
#include <iostream>

pace::task<void> serve() {
    pace::quic::context security(pace::quic::role::server);
    security.use_certificate_chain("tests/certs/localhost.pem", "tests/certs/localhost-key.pem");
    security.set_alpn_protocols({"h3"});
    auto listener = pace::quic::listen({"127.0.0.1", 9094}, security);
    pace::http::router routes;
    routes.get("/health", [] { return pace::http::text("ok\n"); });
    co_await pace::http3::serve_connection(co_await listener.accept(), routes);
}
int main() {
    try { pace::run(serve); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

listener 在当前 context 上绑定，并保持到连接结束。再写客户端：
数字地址决定连接位置，localhost 仍用于证书身份验证。

```cpp
#include <pace/http3.hpp>
#include <pace/run.hpp>
#include <iostream>

pace::task<pace::http::response> get_health() {
    pace::quic::context trust;
    trust.load_verify_file("tests/certs/ca.pem");
    trust.set_alpn_protocols({"h3"});
    auto connection = co_await pace::quic::connect(trust, {"127.0.0.1", 9094}, "localhost");
    co_return co_await pace::http3::async_get(std::move(connection), "localhost:9094", "/health");
}
int main() {
    try {
        auto reply = pace::run(get_health);
        std::cout << reply.status << " " << reply.body;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

先运行 h3_server，再在另一个终端运行 h3_client，应输出 200 ok，两端随后退出。
下一次请求前重新启动服务端。若要持续接收，把
`serve_connection(co_await listener.accept(), routes)` 换成 `serve(listener, routes)`。
测试证书只用于这个本地练习。

本地 QUIC 交换中，客户端读到回复 FIN 后仍保持存活，直到服务端观察到确认包。
配套程序通过等待 connection 事件保留客户端到两端完成；收到 FIN 不等于远端发送任务已经结束。
