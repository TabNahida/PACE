<!-- source-hash: 86b2e9b7ac75 -->
# HTTP/1 服务器

[English](server.md) · [HTTP/1](README.zh.md)

## 概述

`serve` 在监听器上接受连接，并为每个请求运行处理器；`serve_connection` 服务一条已经打开的连接。处理器有两种形式：缓冲式的 `handler`，以及配合 `server_options` 使用的 `server_handler`，后者增加了读取正文之前的准入、按请求的正文上限、接收与写出截止时间，以及由 `body_source` 流式产生的响应。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/http1.hpp>` | `pace_http1` | `--http1=y`（默认）；`tls::stream` 与 `tls::context` 重载需要 `--tls=y` |

请求是缓冲的：在限制范围内收齐整个请求（包括正文和 trailer）之后才运行处理器。消息规则和分帧错误见 [HTTP/1 页面](README.zh.md)。

## 概要

```cpp
#include <pace/http1.hpp>

#include <chrono>
#include <string>

pace::task<pace::http::server_response> upload(pace::http::request request)
{
    co_return pace::http::text("received " + std::to_string(request.body.size()) + " bytes\n");
}

pace::task<void> run_server(pace::tcp::listener& listener)
{
    pace::http::server_options options;
    options.admit = [](const pace::http::request_head& head)
    {
        if (head.header("Authorization") != "Bearer example")
        {
            return pace::http::admission::reject(pace::http::text("unauthorized\n", 401));
        }
        // A larger body limit for one path, the default for the others.
        return head.target == "/upload" ? pace::http::admission::accept(16 * 1024 * 1024)
                                        : pace::http::admission::accept();
    };
    options.request_timeout = std::chrono::seconds(15);
    options.write_timeout = std::chrono::seconds(30);
    co_await pace::http::serve(listener, upload, options);
}
```

## 类型

### `handler`, `server_handler`, `server_response`

```cpp
using handler = std::function<task<response>(request)>;  // from <pace/http/types.hpp>
using server_response = std::variant<response, streaming_response>;
using server_handler = std::function<task<server_response>(request)>;
```

`handler` 返回缓冲的 `response`；[`router`](../http/router.zh.md) 或经 `make_handler` 适配的 lambda 都是 `handler`。`server_handler` 可以返回缓冲的 `response`，也可以返回 `streaming_response`。`handler` 重载接收 `limits`，`server_handler` 重载接收 `server_options`。

### `server_options`

```cpp
struct server_options
{
    limits bounds{};
    std::function<admission(const request_head&)> admit{};
    std::chrono::milliseconds request_timeout{0};
    std::chrono::milliseconds write_timeout{0};
    std::size_t response_buffer_bytes = std::size_t{64} * 1024;
};
```

| 成员 | 含义 |
|:--|:--|
| `bounds` | 接收[限制](README.zh.md#limits) |
| `admit` | 对每个请求在其头部校验通过之后、读取正文之前调用。为空表示全部接受，正文上限为 `bounds.max_body_bytes` |
| `request_timeout` | 接收一个请求的预算；零表示禁用 |
| `write_timeout` | 写出一个响应的预算；零表示禁用 |
| `response_buffer_bytes` | 交给 `body_source::read` 的最大缓冲区；不得为零 |

**请求截止时间。** 在读取请求头之前开始计时（因此包括在 keep-alive 连接上等待下一个请求的时间），在正文和 trailer 收齐或准入拒绝时结束，包含准入回调的时间。这是绝对预算而非空闲计时器：零星到达的字节不会延长它。

**写出截止时间。** 在响应就绪、编码响应头之前开始，涵盖响应的每个字节以及每次 `body_source::read` 调用。错误应答和准入拒绝同样受它约束。它在优雅关闭之前结束，优雅关闭有自己的上限。

截止时间到期时，连接被关闭，挂起的操作被取消。不会发送 408 或其他应答，已经写出一部分的响应保持不完整。两个截止时间都不覆盖处理器、TCP accept 和 TLS 握手：处理器的工作需要自行限制，例如使用 `with_deadline`。同步回调（`admit`、`body_source::read`）无法被打断，到期检查在它返回之后进行。各阶段结束时计时器即被取消，因此长时间的 keep-alive 连接不会积累计时器。

### `request_head`

```cpp
struct request_head
{
    std::string_view method;
    std::string_view target;
    std::string_view version;
    std::span<const header_field> headers;
    std::optional<std::size_t> content_length;
    bool chunked = false;
    bool keep_alive = true;
    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;
};
```

传给 `admit` 的、已经过校验的请求头。请求行、头部字段和分帧已经通过 [HTTP/1 页面](README.zh.md#错误)中的检查；格式错误的输入永远不会进入准入。`content_length` 是声明的长度，chunked 请求或没有长度的请求为空。`header(name)` 返回第一个名字在忽略 ASCII 大小写时匹配的字段值，没有则返回空视图。所有视图和 span 只在 `admit` 调用期间有效，需要保留的内容请复制。此时还没有消费任何正文字节，但最后一次套接字读取（最多 8 KiB）可能已经包含部分正文或流水线中的后续字节。

### `admission`

```cpp
struct admission
{
    std::optional<std::size_t> max_body_bytes;
    std::optional<response> rejection;

    static admission accept(std::optional<std::size_t> maximum = {});
    static admission reject(response result);
};
```

`admit` 返回的决定。

- `accept(n)` 以 `n` 字节为上限读取本请求的正文，只对这个请求生效。`n` 可以大于也可以小于 `bounds.max_body_bytes`。`accept()` 沿用 `bounds.max_body_bytes`；`accept(0)` 只允许空正文。超过上限的 Content-Length 在读取正文之前就以 413 应答；chunked 正文在每个分块大小行处检查，在读取该分块之前。其他限制不变，这个上限也不会延续到同一连接上的下一个请求。
- `reject(result)` 立即发送 `result`，不读取正文也不运行处理器，并且无论 `result.close` 为何值都会关闭连接。随后的优雅关闭可能丢弃未读的正文。

### `body_source`

```cpp
class body_source
{
public:
    virtual ~body_source() = default;
    virtual std::size_t read(std::span<char> destination) = 0;
};
```

按需产生 `streaming_response` 的正文。`read` 最多填充 `destination.size()` 个字节，并返回写入的字节数。返回值必须为正，且不大于 span 的大小。span 不会大于 `response_buffer_bytes` 或剩余应发的字节数。只有在前一批字节写出之后才会再次调用 `read`，因此服务器在内存中最多只持有正文的一个缓冲区（内核和 TLS 另有各自有界的缓冲）。产生满 Content-Length 字节之后不再调用它。

`read` 在连接的事件循环上同步运行，必须很快返回：它适合生成数据和读取已经打开的文件，不适合阻塞的网络或磁盘操作。连接断开、取消或写出截止时间到期后不再调用它，并销毁这个源。

### `streaming_response`

```cpp
struct streaming_response
{
    response head;
    std::uint64_t content_length = 0;
    std::unique_ptr<body_source> source;
};
```

正文来自 `source` 的响应，在 HTTP/1.0 和 HTTP/1.1 上以 `Content-Length: content_length` 分帧。`head` 提供状态码、内容类型和头部字段；它的 `body` 和 `trailers` 必须为空，`chunked` 必须为 false。不支持无长度的流式响应和流式 trailer。当不会读取任何字节时 `source` 可以为空：长度为零、`HEAD` 请求，或状态码为 204、205、304。对 `HEAD` 仍会声明 Content-Length。在所有路径上（包括从未读取的情况）都会销毁这个源。

```cpp
#include <pace/http1.hpp>

#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

class file_body final : public pace::http::body_source
{
public:
    explicit file_body(const std::string& path)
        : file_(path, std::ios::binary | std::ios::ate)
    {
        if (!file_)
        {
            throw std::runtime_error("cannot open snapshot");
        }
        const auto end = file_.tellg();
        if (end < 0)
        {
            throw std::runtime_error("cannot size snapshot");
        }
        size_ = static_cast<std::uint64_t>(end);
        file_.seekg(0);
    }

    std::uint64_t size() const noexcept
    {
        return size_;
    }

    std::size_t read(std::span<char> destination) override
    {
        file_.read(destination.data(), static_cast<std::streamsize>(destination.size()));
        if (file_.bad())
        {
            throw std::runtime_error("cannot read snapshot");
        }
        return static_cast<std::size_t>(file_.gcount());
    }

private:
    std::ifstream file_;
    std::uint64_t size_ = 0;
};

pace::task<pace::http::server_response> backup(pace::http::request)
{
    auto file = std::make_unique<file_body>("backup.tar");
    pace::http::streaming_response result;
    result.head.content_type = "application/octet-stream";
    result.content_length = file->size();
    result.source = std::move(file);
    co_return result;
}
```

长度取自读取正文所用的同一个已打开文件。保持文件打开并不能阻止其他写入者修改它；如果文件变短，`read` 会提前返回零，连接随之关闭。

## 函数

### `serve_connection`

```cpp
task<void> serve_connection(tcp::stream stream, handler handle, limits bounds = {});
task<void> serve_connection(tcp::stream stream, server_handler handle, server_options options = {});
// With PACE_HAS_TLS:
task<void> serve_connection(tls::stream stream, handler handle, limits bounds = {});
task<void> serve_connection(tls::stream stream, server_handler handle, server_options options = {});
```

在一条连接上服务请求，直到连接关闭。task 拥有这个流和处理器。它会开启 `TCP_NODELAY`。`tls::stream` 必须已经完成握手，且协商出的 ALPN 协议必须是 `http/1.1` 或没有；否则结束这条连接。请求逐个读取、处理和应答；已经收到的流水线请求会被保留并按顺序服务。在结束连接的响应之后，流会被优雅关闭（半关闭，然后最多丢弃 100 ms、64 KiB 的输入）。

传输错误、TLS 错误和取消只结束这一条连接：task 正常完成。`server_options` 重载在开始时对负的超时或为零的 `response_buffer_bytes` 抛出 `std::invalid_argument`。

### `serve`

```cpp
task<void> serve(tcp::listener& listener, handler handle, limits bounds = {});
task<void> serve(tcp::listener& listener, server_handler handle, server_options options = {});
task<void> serve(io_context& context, tcp::endpoint endpoint, handler handle, limits bounds = {});
// With PACE_HAS_TLS:
task<void> serve(tcp::listener& listener, const tls::context& configuration, handler handle, limits bounds = {});
task<void> serve(
    tcp::listener& listener,
    const tls::context& configuration,
    server_handler handle,
    server_options options = {}
);
```

循环接受连接，为每条连接在独立的 task 中运行 `serve_connection`，并使用处理器的副本。所有已接受的连接都属于这次调用：停止接受时它们被取消，`serve` 要等它们全部结束后才返回。

- 监听器重载借用监听器，监听器必须比 task 活得更久。监听器被关闭或 task 被取消（例如停止 context）时停止接受，`serve` 随即正常返回。其他 accept 错误会被抛出。
- TLS 重载用 `configuration` 包装每个已接受的套接字，并在该连接自己的 task 中进行握手，因此一次缓慢的握手不会拖慢其他客户端。调用前配置好证书（如果提供 ALPN，还要包含 `http/1.1`）。握手失败只结束那一条连接。
- `io_context&, tcp::endpoint` 重载自行创建并拥有监听器。它在 task 开始运行时才绑定，而不是在创建时，绑定错误从 task 中抛出。它只用于基于 TCP 的缓冲式 `handler`；如果需要立即得到绑定错误、得知临时端口号、使用 TLS、使用 `server_options` 或单独关闭监听器，请自己创建监听器。

没有连接数上限，没有连接注册表，也没有内置的进行中请求排空：关闭监听器会取消它所接受的连接。需要自定义关闭策略时，自己编写 accept 循环，并用 `with_task_group` 管理这些连接（见[结构化并发](../runtime/structured-concurrency.zh.md)）。

## 错误

| 情况 | 结果 |
|:--|:--|
| 请求格式错误或超限 | 按[分帧表](README.zh.md#错误)中的状态码应答，然后关闭 |
| 处理器抛出异常（包括空的 `std::function`） | 500 `Internal Server Error\n`，然后关闭 |
| 处理器抛出错误码为 `operation_canceled` 的 `std::system_error` | 不应答，结束连接 |
| `admit` 抛出异常 | 500，然后关闭；`operation_canceled` 则结束连接 |
| 响应元数据无效（状态码不在 200–599、受管或无效字段、不带 `chunked` 的 trailer、流式 `head` 带正文） | 以 500 代替该响应，然后关闭 |
| 流式响应：`read` 提前返回 0、返回过多字节或抛出异常 | 关闭连接；在部分响应之后不会再写第二个响应 |
| 请求或写出截止时间到期 | 关闭连接，不应答 |
| 传输或 TLS 错误 | 结束连接；`serve_connection` 正常完成 |
| 取消或监听器关闭以外的 accept 错误 | 从 `serve` 抛出 |
| `server_options` 无效 | `serve` 或 `serve_connection` 抛出 `std::invalid_argument` |
| 绑定错误（`serve(context, endpoint, …)`） | task 抛出 `std::system_error` |

一条连接的错误不会停止其他连接。没有日志钩子。已写出的应答只表示本地套接字接受了它，不代表对端一定收到。

## 线程与并发

一切都在监听器的 context 上运行。同一连接上的请求依次处理；各连接是独立的 task，在该 context 上交错执行。要使用多个线程，请在每个 worker context 上各运行一个监听器和一个 `serve`（见[应用页面](app.zh.md)中的 `app::run_workers`）；这时处理器副本会在不同线程上同时运行，必须同步它们共享的状态。`admit` 和 `body_source::read` 是在事件循环线程上的同步调用。

## 生命周期与所有权

- `serve_connection` 拥有它的流、处理器和选项。
- `serve` 把处理器复制到每条连接中。它借用监听器，TLS 重载还借用 `tls::context`；二者以及运行 task 的 context 都必须比它活得更久。
- `streaming_response` 拥有它的 `body_source`，直到正文写完或连接失败。
- `request_head` 中的视图只在 `admit` 调用期间有效。

## 限制与默认值

| 项目 | 默认值 |
|:--|:--|
| `limits` | 见 [HTTP/1 页面](README.zh.md#限制与默认值) |
| `server_options::request_timeout` | 0（禁用） |
| `server_options::write_timeout` | 0（禁用） |
| `server_options::response_buffer_bytes` | 64 KiB |
| `server_options::admit` | 空：所有请求都以 `bounds.max_body_bytes` 接受 |
| 套接字读取大小 | 8 KiB |
| 最后一个响应之后的优雅关闭 | 最多 100 ms，最多丢弃 64 KiB 输入 |
| 并发连接数 | 不限 |

## 参见

- [HTTP/1](README.zh.md)、[HTTP/1 客户端](client.zh.md)、[HTTP/1 应用](app.zh.md)
- [路由](../http/router.zh.md)、[网关服务](../gateway/README.zh.md)
- [TCP](../net/tcp.zh.md)、[TLS](../tls/README.zh.md)、[结构化并发](../runtime/structured-concurrency.zh.md)
- 教程：[HTTP 服务器](../../tutorial/03-http-server.zh.md)、[优雅关闭](../../tutorial/09-graceful-shutdown.zh.md)
