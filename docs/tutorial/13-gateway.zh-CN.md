# 13. 挂载静态目录与代理 API

[English](13-gateway.md) · [学习路线](README.zh-CN.md)

本章使用 PACE 0.5.0 的网关接口，保留默认开启的 HTTP/1。

## 先挂一个目录

创建 `public/index.html`，写入 `<h1>Hello from PACE</h1>`。程序只需要：

```cpp
#include <pace/http.hpp>
int main() {
    pace::http::app app;
    app.mount("/", "./public");
    app.run(8080);
}
```

沿用[第 1 章](01-build.zh-CN.md)的 xmake 工程，链接 `pace_http1`。
在包含 `public` 的目录运行，打开 `http://127.0.0.1:8080/`，即可看到页面。
改成 `app.mount("/assets", "./public")` 后，访问 `/assets/index.html`。
服务不会列出目录，也不会开放挂载目录之外的文件。

## 再代理一个 API

另开一个程序作为上游：

```cpp
#include <pace/http.hpp>
int main() {
    pace::http::app origin;
    origin.get("/hello", [] { return pace::http::text("hello from the API\n"); });
    origin.run(9000);
}
```

在前一个程序的 `run` 前添加：

```cpp
app.proxy("/api", "http://127.0.0.1:9000");
```

访问 `http://127.0.0.1:8080/api/hello` 就能看到上游响应。默认去掉 `/api` 前缀，
上游收到 `/hello`。`/api` 比 `/` 更具体，优先匹配；精确路由又优先于目录匹配。

如果两个上游都提供相同 API，直接传入列表即可轮询分配请求：

```cpp
app.proxy("/api", {"http://127.0.0.1:9000", "http://127.0.0.1:9001"});
```

连接会复用；失败请求不会自动重放。

## 按需开启缓存

默认不缓存。把最初的目录挂载改成：

```cpp
using namespace std::chrono_literals;
pace::http::static_options files;
files.cache.max_bytes = 16 * 1024 * 1024;
files.cache.ttl = 10s;
files.cache_control = "public, max-age=10";
app.mount("/", "./public", files);
```

这样 PACE 可以复用文件内容十秒，期间文件修改可能暂不可见。
`cache_control` 是发给下游的 HTTP 响应头，和 PACE 自身缓存独立；只设置它不会启用内存缓存。

代理也可以设置缓存、超时和最少连接数分配：

```cpp
pace::http::proxy_options proxy;
proxy.balance = pace::http::load_balance::least_connections;
proxy.timeout = 2s;
proxy.cache.max_bytes = 32 * 1024 * 1024;
proxy.cache.ttl = 10s;
app.proxy("/api", {"http://127.0.0.1:9000", "http://127.0.0.1:9001"}, proxy);
```

上游需要返回 `Cache-Control: public, max-age=10` 才会缓存相应的普通 GET。
携带认证、Cookie 的请求，以及带 `Set-Cookie`、`Vary` 的响应，都不会进入这个共享缓存。

## 最后增加 workers

Linux/macOS/FreeBSD 可以把 `app.run(8080)` 换成：

```cpp
app.run_workers(4, {"0.0.0.0", 8080});
```

每个 worker 拥有独立 reactor、监听器和上游连接池。该辅助入口通过 `SO_REUSEPORT`
监听并阻塞运行；Windows 上目前只支持一个 worker。需要自己控制停止时，按照
[第 6 章](06-shutdown.zh-CN.md)把 `app.serve(listener)` 放进已有 runtime。

练习：改变目录前缀，用 curl 发送 HEAD 和 Range 请求，再比较开启与关闭缓存的行为。
缓存失效、路径规则、并发契约与各项调优参数放在[网关 reference](../reference/http-gateway.zh-CN.md)。
继续阅读 [I/O 后端选择](../reference/io-backends.md)和 [nginx 本地性能对照](../gateway-performance.md)。
