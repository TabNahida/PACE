<!-- source-hash: db28edbfcfd7 -->
# HTTP 路由

[English](router.md) · [HTTP](README.zh.md)

## 概述

`make_handler` 把普通的可调用对象转换为 `http::handler`，`router` 按精确的方法和路径把请求分派给这类处理器。两者都可用于 HTTP/1、HTTP/2 和 HTTP/3 服务器。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/http.hpp>`（内容在 `<pace/http/router.hpp>`） | `pace_http`（仅头文件） | 无；始终可用 |

`<pace/http1.hpp>` 也包含它。这个路由器只是一个精确匹配的分派器：没有路径参数、通配符、正则表达式、URL 解码、中间件，也不会自动应答 `OPTIONS`。

## 概要

```cpp
#include <pace/http.hpp>
#include <pace/http1.hpp>

pace::http::router make_routes()
{
    pace::http::router routes;
    routes.get("/health", [] { return pace::http::text("ok\n"); });
    routes.post(
        "/echo",
        [](pace::http::request request) -> pace::task<pace::http::response>
        { co_return pace::http::response{.body = std::move(request.body)}; }
    );
    routes.add("DELETE", "/items", [](pace::http::request) { return pace::http::response{.status = 204}; });
    return routes;
}

pace::task<void> serve(pace::tcp::listener& listener)
{
    co_await pace::http::serve(listener, make_routes());
}
```

## 类型

### `router`

```cpp
class router
{
public:
    template <class Function>
    router& add(std::string method, std::string path, Function&& function);
    template <class Function>
    router& get(std::string path, Function&& function);
    template <class Function>
    router& post(std::string path, Function&& function);
    template <class Function>
    router& mount(std::string prefix, Function&& function);

    task<response> operator()(request incoming) const;
};
```

`router` 本身就是处理器：它可以转换为 `http::handler`，因此能传给任何 `serve` 函数，也能被其他处理器包装。默认构造的路由器对所有请求应答 404。

| 成员 | 作用 |
|:--|:--|
| `add(method, path, function)` | 为精确的方法和路径注册 `function`，返回 `*this` |
| `get(path, function)` | `add("GET", path, function)` |
| `post(path, function)` | `add("POST", path, function)` |
| `mount(prefix, function)` | 为 `prefix` 之下的所有路径、所有方法注册 `function` |
| `operator()(request)` | 分派一个请求，返回所选处理器的 task |

`function` 经由 [`make_handler`](#make_handler) 适配，要求相同。

**注册规则。** `method` 必须是非空的 HTTP token；`path` 必须以 `/` 开头，且不含空格、控制字符、`?` 或 `#`。同一方法和路径重复注册会抛出异常。挂载前缀必须以 `/` 开头，且不含空格、控制字符、`?`、`#` 或 `\`；末尾的斜杠会被去掉，所以 `/static/` 与 `/static` 是同一个前缀，重复挂载同一前缀会抛出异常。

**匹配。** 匹配时忽略请求目标中的查询串（从第一个 `?` 开始），但处理器收到的目标保持原样。方法和路径逐字节、区分大小写地比较；百分号转义不会被解码，因此 `/%68ello` 不匹配 `/hello`。对于一个请求：

1. 如果某条路由的路径和方法都与请求一致，就运行它。
2. 对 `HEAD` 请求，如果该路径没有 `HEAD` 路由，就运行该路径的 `GET` 路由。处理器看到的方法是 `HEAD`；协议层写出响应头并丢弃正文。
3. 如果没有路由的路径与请求完全一致，就运行前缀匹配最长的挂载。前缀按完整路径段匹配：`/api` 匹配 `/api` 和 `/api/x`，不匹配 `/apix`。前缀 `/` 匹配所有路径。挂载的处理器收到完整的、未裁剪的目标。
4. 如果既没有路由也没有挂载匹配，应答 404，正文为 `Not Found\n`。
5. 如果该路径有路由但没有这个方法的，应答 405，正文为 `Method Not Allowed\n`，并带 `Allow` 字段，按注册顺序列出该路径的方法；没有显式 `HEAD` 路由时，在 `GET` 之后补上 `HEAD`。

因此精确路径总是优先于挂载，即使方法不匹配也是如此。

**复制。** 复制路由器的代价很小：副本共享同一张路由表。在副本上第一次注册时，该副本会得到自己的表，所以在副本上添加的路由不会改变原路由器。可调用对象本身仍在副本之间共享。服务器会为每个连接复制一次处理器，共享路由表使这次复制无需分配内存。

## 函数

### `make_handler`

```cpp
template <class Function>
handler make_handler(Function&& function);
```

把 `function` 适配为 `http::handler`。调用 `function` 时要么传入 `request`，要么不传参数（两者都可行时使用传入请求的形式），返回类型必须恰好是 `response` 或 `task<response>`，否则由 `static_assert` 报编译错误。适配器接管可调用对象的所有权（包括带有仅可移动捕获的对象），并让它在每一次已经开始的调用期间保持存活，即使适配器或路由器在调用挂起时被销毁也是如此。引用捕获的生命周期不会被延长，其所指对象必须比服务器活得更久。

调用是惰性的：`function` 在返回的 task 第一次恢复时才运行，它抛出的异常保存在 task 中，并在 `co_await` 时重新抛出。返回 `response` 的函数在服务器的事件循环上同步运行，不得阻塞。

`<pace/http/router.hpp>` 还声明了 `call_handler`、`invoke_handler`、`invoke_function` 和 `immediate_response`。它们是 `make_handler` 和 `router` 的实现，不由 `pace.http` 模块导出，不属于 API。

## 错误

| 情况 | 结果 |
|:--|:--|
| 方法、路径或前缀无效，路由或挂载重复 | `add`/`get`/`post`/`mount` 抛出 `std::invalid_argument`；路由器保持不变 |
| 可调用对象的返回类型不对 | 编译错误 |
| 所选处理器抛出异常 | 从返回的 task 中传播出去。服务器将其转换为自己的错误应答（HTTP/1 为 500） |
| 没有匹配 | 404 或 405 响应，而不是异常 |

## 线程与并发

在把路由器交给服务器之前完成所有路由注册。同一路由器（或仍与它共享路由表的副本）正在服务时再注册是不支持的。分派本身只读取路由表，因此同一路由器的多个副本可以在不同工作线程上并发分派。处理器在这些线程之间共享，它们持有或捕获的任何状态都必须由应用自行同步。

## 生命周期与所有权

路由器拥有它的路由和适配后的可调用对象，副本之间共享。按值接收路由器的服务器持有自己的副本，因此之后销毁或修改原路由器不会影响它。

## 限制与默认值

| 项目 | 值 |
|:--|:--|
| 路径查找 | 在排好序的路径上做一次二分查找，再扫描该路径的方法 |
| 挂载查找 | 线性扫描挂载，仅在没有精确路径时进行 |
| 空路由器的默认响应 | 404 `Not Found\n` |
| `get` / `post` 的方法 | `GET` / `POST` |

## 参见

- [HTTP 消息](README.zh.md)与 [URL 客户端](clients.zh.md)
- [HTTP/1 应用](../http1/app.zh.md)：`http::app` 在内部路由器上注册路由
- [网关服务](../gateway/README.zh.md)：`static_files` 与 `reverse_proxy` 通过 `mount` 挂载
- 教程：[HTTP 服务器](../../tutorial/03-http-server.zh.md)
