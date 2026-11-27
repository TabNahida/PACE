<!-- source-hash: 1a3a6b77ade4 -->
# HTTP 消息与处理器

[English](README.md) · [参考](../README.zh.md)

## 概述

HTTP 中与协议版本无关的部分：HTTP/1、HTTP/2 和 HTTP/3 共用的请求与响应类型、处理器类型和路由器，因此同一个处理器可以服务所有协议版本。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/http.hpp>` | `pace_http`（仅头文件） | 无；始终可用 |

具名模块为 `pace.http`。所有名字都在命名空间 `pace::http` 中；`pace::http1` 是同一命名空间的别名。本模块不含网络代码，各协议模块在这些类型之上提供自己的服务器和客户端：

| 协议 | 头文件 | 命名空间 | 参考 |
|:--|:--|:--|:--|
| HTTP/1.0 与 HTTP/1.1 | `<pace/http1.hpp>` | `pace::http` | [HTTP/1](../http1/README.zh.md) |
| HTTP/2 | `<pace/http2.hpp>` | `pace::http2` | [HTTP/2](../http2/README.zh.md) |
| HTTP/3 | `<pace/http3.hpp>` | `pace::http3` | [HTTP/3](../http3/README.zh.md) |

本模块的页面：

| 页面 | 内容 |
|:--|:--|
| 本页 | `header_field`、`request`、`response`、`text`/`html`/`json`、`protocol_error`、`handler` |
| [路由](router.zh.md) | `make_handler`、`router`：处理器适配与精确路由 |
| [URL 客户端](clients.zh.md) | HTTP/1、HTTP/2、HTTP/3 中的 `get`/`post`/`fetch` 与 `async_get`/`async_post`/`async_fetch` |

## 概要

```cpp
#include <pace/http.hpp>
#include <pace/runtime.hpp>

int main()
{
    pace::http::router routes;
    routes.get("/hello", [] { return pace::http::text("hello\n"); });

    // The same router can be passed to pace::http::serve, pace::http2::serve or pace::http3::serve.
    // Here it is called directly.
    auto reply = pace::run(routes(pace::http::request{.method = "GET", .target = "/hello"}));
    return reply.status == 200 && reply.body == "hello\n" ? 0 : 1;
}
```

## 类型

### `header_field`

```cpp
struct header_field
{
    std::string name;
    std::string value;
};
```

一个头部字段或 trailer 字段，两个字符串都拥有各自的内容。字段名保持线路上或应用给出的原样拼写；HTTP/2 和 HTTP/3 收到的字段名为小写。重复字段按顺序保留为独立条目，PACE 从不合并它们。

### `request`

```cpp
struct request
{
    std::string method;
    std::string target;
    std::string version;
    std::vector<header_field> headers{};
    std::string body;
    bool keep_alive = true;
    std::vector<header_field> trailers{};
    bool chunked = false;

    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;
};
```

| 成员 | 含义 |
|:--|:--|
| `method` | 原样的方法 token，区分大小写（`GET`、`POST` 等） |
| `target` | origin 形式的请求目标，即路径加可选查询串（`/a/b?x=1`），从不做 URL 解码 |
| `version` | HTTP/1 上为 `HTTP/1.0` 或 `HTTP/1.1`；收到的 HTTP/2、HTTP/3 请求为 `HTTP/2` 或 `HTTP/3`。HTTP/1 客户端把空版本视为 `HTTP/1.1` |
| `headers` | 协议自行管理的字段以外的头部字段。收到的 HTTP/2、HTTP/3 请求中，`:authority` 伪头部以 `host` 字段出现 |
| `body` | 已去除分帧的完整正文（不含分块边界），可以是二进制 |
| `keep_alive` | 仅 HTTP/1：本次交换后是否保持连接 |
| `trailers` | trailer 字段，与 `headers` 分开存放 |
| `chunked` | 仅 HTTP/1：正文已经或将要以 chunked 传输编码发送 |

`header(name)` 返回 `headers` 中第一个名字与 `name` 在忽略 ASCII 大小写时相等的字段值，没有时返回空视图，因此缺失的字段与空值字段无法区分。该视图指向请求内部，请求被修改或销毁前应先复制。不查找 trailer。

### `response`

```cpp
struct response
{
    unsigned status = 200;
    std::string body;
    std::string content_type = "text/plain; charset=utf-8";
    std::vector<header_field> headers{};
    bool close = false;
    std::string version = "HTTP/1.1";
    std::vector<header_field> trailers{};
    bool chunked = false;
    std::optional<std::uint64_t> representation_length{};

    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;
};
```

| 成员 | 含义 |
|:--|:--|
| `status` | 状态码。服务器响应必须在 `[200, 599]` 内 |
| `body` | 完整正文 |
| `content_type` | 作为 `Content-Type` 字段发送；空字符串表示不发送。收到的响应中为对端的 `Content-Type` 值，或为空。内容类型只能在这里设置，不能通过 `headers` |
| `headers` | 其他头部字段。分帧字段（`Content-Length`、`Transfer-Encoding`、`Connection`、`Trailer`）由协议生成，不得加入 |
| `close` | 仅 HTTP/1。服务器响应中为 `true` 表示发送后关闭连接；收到的响应中为 `true` 表示连接不能复用 |
| `version` | 收到的响应中为对端版本（`HTTP/1.0`、`HTTP/1.1`、`HTTP/2`、`HTTP/3`）。服务器响应忽略此字段：HTTP/1 服务器总是写出 `HTTP/1.1` 状态行 |
| `trailers` | trailer 字段。HTTP/1 只在 `chunked = true` 时发送 |
| `chunked` | 仅 HTTP/1：以 chunked 传输编码发送正文；收到的响应中表示它是否以分块方式到达 |
| `representation_length` | 仅 HTTP/1，用于 `HEAD` 的应答：`body` 为空时要声明的 `Content-Length`。供反向代理使用；携带正文的响应忽略它 |

`header(name)` 的行为与 `request` 相同。

标为“仅 HTTP/1”的字段会被 HTTP/2 和 HTTP/3 忽略，因为它们没有逐消息的连接级分帧。各协议写出和读取这些字段时的规则见各自的页面。

### `protocol_error`

```cpp
class protocol_error : public std::runtime_error
{
public:
    explicit protocol_error(const std::string& message);
};
```

客户端在对端响应格式错误、被截断或超出所配置的限制时抛出。抛出时连接已经关闭。传输层失败（包括取消）则是 `std::system_error`。

### `handler`

```cpp
using handler = std::function<task<response>(request)>;
```

HTTP/1、HTTP/2 和 HTTP/3 服务器端的处理器。它按值接收完整请求，返回一个产生完整响应的 task。可以直接写成接收 `request` 的协程，也可以由 [`make_handler` 或 `router`](router.zh.md) 适配返回 `response` 或 `task<response>` 的 lambda。HTTP/1 服务器还接受可以流式发送响应的 `server_handler`，见 [HTTP/1 服务器](../http1/server.zh.md)。

### `pace::http1`

```cpp
namespace pace
{
namespace http1 = http;
}
```

显式写出 HTTP/1 版本的别名：`pace::http1::get` 与 `pace::http::get` 是同一个函数。

## 函数

### `text`, `html`, `json`

```cpp
inline response text(std::string body, unsigned status = 200);
inline response html(std::string body, unsigned status = 200);
inline response json(std::string body, unsigned status = 200);
```

返回拥有 `body` 的 `response`，带给定状态码，内容类型分别为 `text/plain; charset=utf-8`、`text/html; charset=utf-8` 或 `application/json`。它们不做任何转义、序列化或校验：`json` 接收的是已经是 JSON 的文本。其余字段保持默认值，可以在结果上修改。

路由函数 `make_handler` 与 `router` 见[路由页](router.zh.md)。

## 错误

本页的类型和辅助函数除 `std::bad_alloc` 外不抛出异常。`protocol_error` 是客户端用来报告对端响应格式错误的异常。各服务器如何处理处理器抛出的异常见对应协议的页面；HTTP/1 服务器以 500 应答并关闭连接。

## 线程与并发

`request` 和 `response` 是普通值，没有线程亲和性。服务器把 `handler` 复制到每个连接中，这些副本可能同时运行在不同的工作线程上，因此处理器捕获中共享的状态必须由应用自行同步。

## 生命周期与所有权

消息的每个成员都拥有自己的数据。`header()` 返回的视图借用消息内部的存储。处理器按值接收请求，在整个协程期间拥有它，可以随意移出其内容。

## 限制与默认值

| 项目 | 默认值 |
|:--|:--|
| `response::status` | 200 |
| `response::content_type` | `text/plain; charset=utf-8` |
| `response::version` | `HTTP/1.1` |
| `request::keep_alive` | `true` |
| `request::chunked`、`response::chunked`、`response::close` | `false` |

接收消息的大小限制属于各协议：HTTP/1 为 `http::limits`，另有 `http2::limits` 和 `http3::limits`。

## 参见

- [路由](router.zh.md)与 [URL 客户端](clients.zh.md)
- [HTTP/1](../http1/README.zh.md)、[HTTP/2](../http2/README.zh.md)、[HTTP/3](../http3/README.zh.md)
- [网关服务](../gateway/README.zh.md)
- 教程：[HTTP 客户端](../../tutorial/02-http-client.zh.md)、[HTTP 服务器](../../tutorial/03-http-server.zh.md)
- [HTTP 实用做法](../../guides/http-recipes.zh.md)
