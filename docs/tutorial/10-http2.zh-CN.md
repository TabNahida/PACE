# 10. 发出第一个 HTTP/2 请求

[English](10-http2.md) · [上一章：WebSocket](09-websocket.zh-CN.md) · [下一章：QUIC/HTTP3](11-quic-http3.zh-CN.md)

在第一章的当前源码工程启用 `--http2=y`，依赖 `pace_http2`。
沿用第三章的 target 写法，创建 `h2_server`、`h2_client`。
先用明文 HTTP/2 prior knowledge，不进行 HTTP/1 Upgrade。

## 第一步：保留路由，更换协议

保存为 h2_server.cpp：

```cpp
#include <pace/http2.hpp>
#include <pace/http_router.hpp>
#include <pace/run.hpp>
#include <iostream>

pace::task<void> serve() {
    auto listener = pace::tcp::listen({"127.0.0.1", 9093});
    pace::http::router routes;
    routes.get("/health", [] { return pace::http::text("ok\n"); });
    co_await pace::http2::serve_connection(co_await listener.accept(), routes);
}
int main() {
    try { pace::run(serve); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

路由和响应 helper 与 HTTP/1 一样。`serve_connection` 接管一个已接受的连接，
运行 HTTP/2 到对端断开。本练习随后退出；持续接收使用 `http2::serve(listener, routes)`。

## 第二步：直接取得响应

保存为 h2_client.cpp：

```cpp
#include <pace/http2.hpp>
#include <iostream>

int main() {
    try {
        auto reply = pace::http2::get("http://127.0.0.1:9093/health");
        std::cout << reply.status << " " << reply.body;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`http2::get(url)` 直接返回响应，默认预算 30 秒。协程内写
`co_await http2::async_get(url)`。域名需要 dns=y，HTTPS 需要 tls=y。
详见[统一客户端](../reference/http-clients.zh-CN.md)。

## 第三步：核对，再增加请求

构建两个目标，先运行 `xmake run h2_server`，再运行 `xmake run h2_client`，
应输出 `200 ok`。下一次请求前重启服务端。换一个未注册路径，检查 404。
仓库的 `xmake run tutorial_http2` 会同时检查单次 GET 和批量请求。

要在一条连接上并发多个请求，再使用
[`async_exchange` 与 handler 限额](../reference/protocol-tuning.zh-CN.md#http2-与-http3)。
循环发单次 GET 会建立独立批次，无法获得同连接多路复用的好处。
增加 TLS 时，按第八章在两端配置 ALPN `h2`，把验证握手后的 TLS stream 交给 transport 形式的 async_get。
服务端 TLS 重载与流控见 [HTTP/2 参考](../reference/http2.md)。
