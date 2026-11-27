<!-- source-hash: e8e1c765e08e -->
# HTTP/1 应用

[English](app.md) · [HTTP/1](README.zh.md)

## 概述

`http::app` 汇集路由、静态目录和反向代理，然后通过 HTTP/1 提供服务：用 `run` 在调用线程上运行，用 `run_workers` 在多个工作线程上运行，或用 `serve` 作为 task 运行在你已有的 context 上。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/http1.hpp>` | `pace_http1` | `--http1=y`（默认） |

`app` 只是 [`router`](../http/router.zh.md)、[`static_files`、`reverse_proxy`](../gateway/README.zh.md) 和缓冲式 [`serve`](server.zh.md) 之上的薄层。它不安装信号处理器，不会自行启动后台线程，也没有中间件、路径参数或 URL 解码。

## 概要

```cpp
#include <pace/http1.hpp>

int main()
{
    pace::http::app app;
    app.get("/", [] { return pace::http::text("hello\n"); })
        .post("/echo", [](pace::http::request request) { return pace::http::text(std::move(request.body)); })
        .mount("/assets", "./public")
        .proxy("/api", "http://127.0.0.1:9000");
    app.run(8080); // http://127.0.0.1:8080, until the process is stopped
}
```

## 类型

### `app`

```cpp
class app
{
public:
    template <class Function>
    app& route(std::string method, std::string path, Function&& function);
    template <class Function>
    app& get(std::string path, Function&& function);
    template <class Function>
    app& post(std::string path, Function&& function);

    app& mount(std::string prefix, std::filesystem::path directory, static_options options = {});
    app& proxy(std::string prefix, std::vector<std::string> upstreams, proxy_options options = {});
    app& proxy(std::string prefix, std::initializer_list<std::string> upstreams, proxy_options options = {});
    app& proxy(std::string prefix, std::string upstream, proxy_options options = {});

    void run_workers(std::size_t workers, tcp::endpoint endpoint, limits bounds = {}) const;
    task<void> serve(io_context& io, tcp::endpoint endpoint, limits bounds = {}) const;
    task<void> serve(tcp::listener& listener, limits bounds = {}) const;
    void run(tcp::endpoint endpoint, limits bounds = {}) const;
    void run(std::uint16_t port = 8080, limits bounds = {}) const;

    [[nodiscard]] router handler() const;
};
```

**配置。** 所有配置成员都返回 `*this`，因此可以链式调用。

| 成员 | 作用 |
|:--|:--|
| `route(method, path, function)` | 即 `router::add`：一条精确路由。`function` 接收 `request` 或不接收参数，返回 `response` 或 `task<response>` |
| `get(path, function)`、`post(path, function)` | `route("GET", …)`、`route("POST", …)` |
| `mount(prefix, directory, options)` | 用挂载到路由器上的 [`static_files`](../gateway/README.zh.md#static_files)，为 `prefix` 之下的路径提供 `directory` 中的文件 |
| `proxy(prefix, upstreams, options)` | 用挂载到路由器上的 [`reverse_proxy`](../gateway/README.zh.md#reverse_proxy)，把 `prefix` 之下的请求转发到一个或多个上游基础 URL |

精确路由优先于挂载和代理，后两者之间前缀匹配最长者优先；完整的[匹配规则](../http/router.zh.md#类型)见路由页面。如需读取静态服务或代理服务的缓存统计、清空其缓存，请自己构造服务，改为挂载到 `router` 上。

**提供服务。**

| 成员 | 作用 |
|:--|:--|
| `serve(io, endpoint, bounds)` | 返回一个 task，开始运行时绑定 `endpoint` 后提供服务；见[服务器页面](server.zh.md#serve)中的 `serve(io_context&, tcp::endpoint, …)` |
| `serve(listener, bounds)` | 返回一个在你拥有的监听器上提供服务的 task；它借用该监听器 |
| `run(endpoint, bounds)` | 在调用线程上创建一个 context，在其上服务 `endpoint`，直到该 context 停止或服务失败才返回 |
| `run(port, bounds)` | 即 `run({"127.0.0.1", port}, bounds)`：仅回环地址。要监听其他网卡，请传入 `{"0.0.0.0", 8080}` 这样的端点 |
| `run_workers(workers, endpoint, bounds)` | 创建含 `workers` 个 context 的 `runtime`，每个 context 绑定一个监听 `endpoint` 的监听器并运行：context 0 在调用线程上，其余在 `workers - 1` 个新线程上。runtime 停止时返回 |
| `handler()` | 以 `router` 形式返回路由的副本，供 HTTP/2、HTTP/3、TLS、`server_options` 或自定义适配使用 |

`serve` 和 `handler` 在被调用时对路由做快照：之后添加的路由不会进入已经启动的服务器，task 也不引用 `app`。`run` 和 `run_workers` 使用调用时的路由。

多于一个 worker 时，`run_workers` 以 `SO_REUSEPORT`（FreeBSD 上为 `SO_REUSEPORT_LB`）绑定每个监听器，由内核把连接分摊给它们；这在 Linux、macOS 和 FreeBSD 上可用。Windows 上只能用一个 worker。端口为 0 时，第一个监听器选取一个空闲端口，其余监听器绑定同一端口。

## 错误

| 情况 | 异常 |
|:--|:--|
| 方法、路径或前缀无效；路由或挂载重复 | 配置调用抛出 `std::invalid_argument` |
| 静态或代理选项无效，或上游 URL 无效 | `mount` 或 `proxy` 抛出 `std::invalid_argument`（见[网关错误](../gateway/README.zh.md#错误)） |
| `mount` 的目录无法打开 | `mount` 抛出 `std::system_error` |
| 绑定或监听失败 | `run`、`run_workers` 或 `serve(io, endpoint, …)` task 抛出 `std::system_error` |
| 在已经运行 PACE context 的线程上调用 `run` 或 `run_workers` | `std::logic_error` |
| `run_workers(0, …)` | `std::invalid_argument` |
| 在没有 `SO_REUSEPORT` 的平台上以多个 worker 调用 `run_workers` | `std::system_error`，错误码 `std::errc::operation_not_supported` |

单个请求和连接的错误由服务器处理，不会离开 `run`；见[服务器页面](server.zh.md#错误)。

## 线程与并发

在提供服务之前于同一线程上完成配置；配置不做同步。`run` 阻塞调用线程，只在该线程上服务。`run_workers` 同时在 `workers` 个线程上服务：这时所有处理器、静态服务和代理都会在多个线程上并发运行，处理器之间共享的状态必须同步。网关服务本身已经做了同步（见[网关的线程说明](../gateway/README.zh.md#线程与并发)）。

如需从代码中停止 `run` 或 `run_workers`（例如收到信号时），请用 `serve` 配合你自己的 `io_context` 或 `runtime`，然后停止它（见[优雅关闭](../../tutorial/09-graceful-shutdown.zh.md)）。

## 生命周期与所有权

app 拥有它的路由器。`serve`、`run` 和 `handler` 持有它的副本而不借用 app，因此在 `serve` task 仍在运行时可以销毁 app。`serve(listener, …)` 借用监听器，监听器必须比 task 活得更久。处理器的捕获与持有它们的路由同生命周期。

## 限制与默认值

| 项目 | 默认值 |
|:--|:--|
| `run` 的端口 | 8080 |
| `run(port)` 的地址 | `127.0.0.1` |
| `limits` | 见 [HTTP/1 页面](README.zh.md#限制与默认值) |
| 监听队列（backlog） | 1024 |
| worker 监听器 | 每个 context 一个，`workers > 1` 时使用 `SO_REUSEPORT` |
| `static_options`、`proxy_options` | 见[网关默认值](../gateway/README.zh.md#限制与默认值) |

## 参见

- [路由](../http/router.zh.md)、[HTTP/1 服务器](server.zh.md)、[网关服务](../gateway/README.zh.md)
- [`pace::run`](../runtime/run.zh.md)、[runtime 与 worker](../runtime/runtime.zh.md)
- 教程：[HTTP 服务器](../../tutorial/03-http-server.zh.md)、[worker 与线程](../../tutorial/08-workers-and-threads.zh.md)、[网关](../../tutorial/14-gateway.zh.md)
