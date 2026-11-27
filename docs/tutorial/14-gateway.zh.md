<!-- source-hash: 96feda534fe3 -->
# 14. 静态文件与反向代理

[English](14-gateway.md) · [学习路线](README.zh.md) · [上一章：QUIC 与 HTTP/3](13-quic-and-http3.zh.md)

`http::app` 还可以提供一个目录，并把某个路径前缀转发给其他服务器。这些网关服务随 `pace_http1` 提供（HTTP/1 默认启用），声明在 `<pace/gateway.hpp>` 中，而 `<pace/http1.hpp>` 已经包含了它。

## 挂载一个目录

创建 `public/index.html`，内容为 `<h1>Hello from PACE</h1>`。保存为 `gateway.cpp`，并添加一个依赖 `pace_http1` 的 `gateway` 目标：

```cpp
#include <pace/http1.hpp>

int main()
{
    pace::http::app app;
    app.mount("/", "./public");
    app.run(8080);
}
```

在包含 `public` 的目录中运行它（`xmake run` 在构建目录中启动程序，所以要在项目目录下使用 `xmake run -w . gateway`），然后打开 `http://127.0.0.1:8080/`。`mount` 在被调用时检查目录：如果那里没有 `./public`，它会抛出 `std::system_error`，而这个没有 `catch` 的程序随之终止。`/index.html` 返回同一个文件。如果改为挂载到 `/assets`，`/assets/index.html` 就映射到 `public/index.html`。目录之外的文件绝不会被提供，也不会生成目录列表。

## 添加反向代理

再运行一个 PACE 程序作为 9000 端口上的 API 服务器（`api.cpp`，另一个依赖 `pace_http1` 的目标）：

```cpp
#include <pace/http1.hpp>

int main()
{
    pace::http::app origin;
    origin.get("/hello", [] { return pace::http::text("hello from the API\n"); });
    origin.run(9000);
}
```

在 `gateway.cpp` 中，在 `app.run(8080)` 之前加上：

```cpp
app.proxy("/api", "http://127.0.0.1:9000");
```

现在 `http://127.0.0.1:8080/api/hello` 返回 `hello from the API`。默认会去掉前缀，所以上游收到的是 `/hello`。更具体的 `/api` 挂载优先于 `/`，精确路由又优先于挂载。要把请求分摊到两个提供相同 API 的上游，传入两个 URL：

```cpp
app.proxy("/api", {"http://127.0.0.1:9000", "http://127.0.0.1:9001"});
```

请求轮流发往各个上游（轮询），并复用空闲的上游连接。失败的交换不会在另一个上游上重放。

## 开启有界缓存

缓存默认关闭。包含 `<chrono>`，把静态挂载换成：

```cpp
using namespace std::chrono_literals;
pace::http::static_options files;
files.cache.max_bytes = 16 * 1024 * 1024;
files.cache.ttl = 10s;
files.cache_control = "public, max-age=10";
app.mount("/", "./public", files);
```

现在 PACE 会把文件内容保留最多十秒，所以对文件的修改在这段时间内可能看不到。`cache_control` 为下游缓存设置 `Cache-Control` 头部；它并不会开启 PACE 自己的缓存。缓存预算和文件大小限制是两个独立的设置。

要改为配置代理：

```cpp
pace::http::proxy_options proxy;
proxy.balance = pace::http::load_balance::least_connections;
proxy.timeout = 2s;
proxy.cache.max_bytes = 32 * 1024 * 1024;
proxy.cache.ttl = 10s;
app.proxy("/api", {"http://127.0.0.1:9000", "http://127.0.0.1:9001"}, proxy);
```

只有符合条件的公共 GET 响应才会被存储。要让 `/hello` 可以缓存，让它的处理函数返回带有 `Cache-Control: public, max-age=10` 头部的响应。带 cookie 或 `Vary` 的响应，以及带凭据或 cookie 的请求，都会绕过这个共享缓存。

## 使用多个工作线程

在 Linux、macOS 和 FreeBSD 上，把 `app.run(8080)` 换成：

```cpp
app.run_workers(4, {"0.0.0.0", 8080});
```

每个工作线程都有自己的事件循环、监听器和上游连接池；它们通过 `SO_REUSEPORT` 共享端口。这个调用会阻塞直到关闭，调用线程本身就是四个工作线程之一。在 Windows 上，这个函数只支持一个工作线程。要由应用控制关闭，请在你自己运行的 runtime 上使用 `app.serve(listener)`，做法见[第 9 章](09-graceful-shutdown.zh.md)。

改一改挂载前缀，然后用 curl 发送 HEAD 请求和字节范围请求（`curl -r 0-3 http://127.0.0.1:8080/`）。比较 `files.cache.max_bytes = 0` 与开启缓存时的差别。[网关参考](../reference/gateway/README.zh.md)解释校验器、失效、并发、限制，以及它与完整 nginx 部署的区别。[I/O 后端](../reference/runtime/io-backends.zh.md)和[网关性能测量](../performance/http-gateway.zh.md)介绍进一步的调优。

下一章：[实用指南](../guides/README.zh.md)，教程之外的配方。
