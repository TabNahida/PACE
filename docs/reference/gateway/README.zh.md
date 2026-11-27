<!-- source-hash: 470962b5aaa2 -->
# 网关服务

[English](README.md) · [参考](../README.zh.md)

## 概述

把 PACE 的 HTTP/1 服务器变成小型嵌入式网关的处理器：`static_files` 提供目录中的文件，`reverse_proxy` 带负载均衡地转发到固定的上游服务器，`response_cache` 是二者共用的有界分片缓存。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/gateway.hpp>`（内容在 `<pace/gateway/services.hpp>`） | `pace_http1` | `--http1=y`（默认） |

网关目前没有自己的目标：它编译在 `pace_http1` 中，随 HTTP/1 一起启用，因为 `http::app` 使用它，而代理又使用 HTTP/1 客户端。`<pace/http1.hpp>` 也声明了它。具名模块为 `pace.gateway`；名字位于命名空间 `pace::http`。HTTPS 上游需要 `--tls=y`，以主机名表示的上游需要 `--dns=y`。

两个服务都是普通的 `http::handler` 值。可以用 [`app::mount` 和 `app::proxy`](../http1/app.zh.md) 挂载，也可以用 [`router::mount`](../http/router.zh.md)。一切都是缓冲的：没有配置语言、进程监管、压缩、磁盘缓存、证书管理、主动健康检查，也没有 WebSocket/`CONNECT` 隧道。

## 概要

```cpp
#include <pace/gateway.hpp>
#include <pace/http1.hpp>

#include <iostream>

int main()
{
    pace::http::static_options files;
    files.cache.max_bytes = 64 * 1024 * 1024; // enable the local file cache
    pace::http::static_files assets("/assets", "./public", files);

    pace::http::proxy_options proxy;
    proxy.balance = pace::http::load_balance::least_connections;
    pace::http::reverse_proxy api("/api", {"http://127.0.0.1:9001", "http://127.0.0.1:9002"}, proxy);

    pace::http::router routes;
    routes.mount("/assets", assets).mount("/api", api);
    routes.get("/health", [] { return pace::http::text("ok\n"); });

    pace::run([&](pace::io_context& io) { return pace::http::serve(io, {"127.0.0.1", 8080}, routes); });
    std::cout << assets.cache_stats().hits << " cache hits\n";
}
```

## 类型

### `static_files`

```cpp
class static_files
{
public:
    static_files(std::string prefix, std::filesystem::path directory, static_options options = {});
    task<response> operator()(request incoming) const;
    void clear_cache() const;
    cache_statistics cache_stats() const;
};
```

为 `prefix` 之下的请求路径提供 `directory` 之下的普通文件。构造函数会打开 `directory`，该目录必须存在。副本共享同一个服务及其缓存。

- `operator()` 应答 `GET` 和 `HEAD`；其他方法得到 405 和 `Allow: GET, HEAD`。不在 `prefix` 之下的路径得到 404。前缀之后的部分做一次百分号解码，然后映射到文件。
- 目录通过其 `index` 文件提供，不重定向，也不生成目录列表。链接到目录时请带末尾的 `/`，以便页面中的相对链接正确解析。
- 响应带有按扩展名得出的 `Content-Type`、`ETag`、`Last-Modified`、`Accept-Ranges: bytes`、`X-Content-Type-Options: nosniff`，以及设置了的 `Cache-Control`。ETag 由内容哈希（XXH64）加文件大小组成，因此任何修改都会改变它，包括大小和时间戳都不变的修改。它不是密码学校验和。
- `If-None-Match`（弱或强标签，或 `*`）产生 304；没有它时，`If-Modified-Since` 与 `Last-Modified` 文本完全相等时也产生 304。
- `GET` 支持一个字节范围：`bytes=a-b`、`bytes=a-` 或 `bytes=-n`，返回 206 和 `Content-Range`；无法满足的范围得到 416。多个范围、其他单位，以及与 ETag 或 `Last-Modified` 不完全相等的 `If-Range` 会被忽略，并发送整个文件。`HEAD` 忽略 `Range`。
- `clear_cache()` 清空本地缓存；`cache_stats()` 读取它的计数器。

**路径安全。** 以下情况以 400 拒绝：无效的百分号转义、`.` 和 `..` 路径段、控制字节、反斜杠、Windows 备用数据流（`:`）、以点或空格结尾的路径段，以及隐藏路径段（以 `.` 开头），除非设置了 `allow_dotfiles`。从不跟随符号链接，无论它是文件本身还是中间目录；Windows 上还拒绝 reparse point，并检查打开的文件的最终路径是否位于根目录之下。不存在的文件以及普通文件以外的任何东西得到 404。根目录本身是可信的：只发布其全部内容都可以对外提供的目录。

**读取。** 文件在事件循环上同步读取，以缓冲正文返回，大小上限为 `max_file_bytes`（更大的文件得到 413）。没有 `sendfile`、异步磁盘 I/O 或流式发送。即使缓存命中，也要把正文复制到响应中。请让提供的文件保持较小；大量或冷门的大文件应交给专门的文件服务器。

**本地缓存。** `options.cache.max_bytes` 非零时，200 响应按解码后的路径（忽略查询串）连同验证器一起缓存，直到 TTL 结束或被淘汰。命中时不再检查文件，因此被修改或删除的文件在此之前仍可能从缓存提供；部署后请调用 `clear_cache()`。带 `Cache-Control` 或 `Pragma` 的请求绕过缓存。

### `static_options`

```cpp
struct static_options
{
    std::string index = "index.html";
    std::size_t max_file_bytes = std::size_t{8} * 1024 * 1024;
    bool allow_dotfiles = false;
    std::string cache_control = "no-cache";
    cache_options cache{};
};
```

| 成员 | 含义 |
|:--|:--|
| `index` | 目录对应的文件：一个文件名，不含分隔符、`%`、`.` 或 `..`。为空表示目录得到 404 |
| `max_file_bytes` | 提供的最大文件；更大的文件得到 413 |
| `allow_dotfiles` | 提供以 `.` 开头的路径段 |
| `cache_control` | 发给客户端的 `Cache-Control` 值；为空则不发送。与本地缓存无关 |
| `cache` | 本地缓存；`cache.max_bytes` 为 0 时禁用 |

### `reverse_proxy`

```cpp
class reverse_proxy
{
public:
    reverse_proxy(std::string prefix, std::vector<std::string> upstreams, proxy_options options = {});
    task<response> operator()(request incoming) const;
    void clear_cache() const;
    cache_statistics cache_stats() const;
};
```

把 `prefix` 之下的请求通过 HTTP/1.1 转发给 `upstreams` 之一，并返回上游的响应。副本共享同一个服务及其缓存。

**上游**是不带查询串和片段的绝对 `http://` 或 `https://` 基础 URL，解析方式与[客户端 URL](../http/clients.zh.md#url-规则) 相同。HTTPS 上游依据系统根证书验证，使用 ALPN `http/1.1`。上游只使用 HTTP/1.1；没有 HTTP/2 或 HTTP/3 上游，也不做协议转换。

**转发。** 开启 `strip_prefix` 时，先从目标中去掉前缀（挂载在 `/api` 的 `/api/a?x=1` 变为 `/a?x=1`），再在前面加上上游的基础路径。方法、正文和查询串保持不变。`Host` 改为上游的 authority。逐跳字段、`Connection` 点名的字段、分帧与 trailer 字段，以及传入的 `Forwarded` 和 `X-Forwarded-*` 字段会被删除；不会添加客户端地址，上游需要时请自行添加身份头部。`CONNECT` 和带 `Upgrade` 的请求得到 501。上游响应最多缓冲 `max_response_bytes`，其 trailer 被丢弃，HTTP/1 分帧重新生成。`HEAD` 响应通过 `response::representation_length` 保留上游的 `Content-Length`。

**负载均衡。** 每个 worker context 为每个上游维护自己的连接池和计数器；线程之间除缓存外不共享任何东西。

- `round_robin` 选取下一个既不在冷却期、也未达到 `max_connections` 的上游。
- `least_connections` 在符合条件的上游中选取活动交换最少的一个，平局时轮转。
- 传输失败、超时或响应格式错误会让该上游进入 `fail_timeout` 的冷却期，并丢弃它的空闲连接。HTTP 错误状态码不算失败。
- 空闲的 keep-alive 连接（每个上游最多 `max_idle_connections` 个）会被复用；空闲超过 `idle_timeout` 的连接在下次被选中时丢弃。

不排队也不重试：没有符合条件的上游时立即应答 503，失败的请求不会被重放，即使是遇到失效 keep-alive 连接的 `GET` 也不会。

**代理缓存。** `options.cache.max_bytes` 非零时，代理只在以下条件全部满足时缓存响应：

- 请求是没有正文的 `GET`，目标不超过 4096 字节，且不带 `Authorization`、`Cookie`、`Range`、任何 `If-*` 字段、`Cache-Control` 或 `Pragma`；
- 响应是 200，其 `Cache-Control` 含 `public` 或 `s-maxage`，不含 `private`、`no-cache`、`no-store` 或无法识别的指令，并且没有 `Set-Cookie`、`Vary`、`Content-Range` 或 `Pragma`。

键由下游的 `Host` 加上带查询串的完整目标组成。生存时间取配置的 TTL 与上游新鲜度中较小者，并按其 `Date` 和 `Age` 扣减；缓存命中时带有更新后的 `Age`。这是有意保持精简的 HTTP 缓存子集，并非完整的 RFC 9111 缓存。

### `proxy_options`, `load_balance`

```cpp
enum class load_balance
{
    round_robin,
    least_connections
};

struct proxy_options
{
    load_balance balance = load_balance::round_robin;
    bool strip_prefix = true;
    std::size_t max_connections = 64;
    std::size_t max_idle_connections = 16;
    std::size_t max_response_bytes = std::size_t{8} * 1024 * 1024;
    std::chrono::milliseconds timeout{30000};
    std::chrono::seconds idle_timeout{30};
    std::chrono::milliseconds fail_timeout{1000};
    cache_options cache{};
};
```

| 成员 | 含义 |
|:--|:--|
| `balance` | 上游选择方式，按 worker context 计 |
| `strip_prefix` | 转发前去掉挂载前缀 |
| `max_connections` | 每个上游、每个 worker context 的并发交换数；超过时跳过该上游。至少为 1 |
| `max_idle_connections` | 每个上游、每个 worker context 保留的空闲 keep-alive 连接数；不超过 `max_connections` |
| `max_response_bytes` | 上游正文的最大字节数 |
| `timeout` | DNS、连接、TLS 和交换共用的一个截止时间；必须为正。到期得到 504 |
| `idle_timeout` | 空闲连接超过此时长后丢弃而不复用 |
| `fail_timeout` | 上游失败后的冷却时长 |
| `cache` | 共享的代理缓存；`cache.max_bytes` 为 0 时禁用 |

### `response_cache`

```cpp
class response_cache
{
public:
    explicit response_cache(cache_options options = {});
    ~response_cache();
    response_cache(const response_cache&) = delete;
    response_cache& operator=(const response_cache&) = delete;
    std::shared_ptr<const response> get(std::string_view key, std::chrono::seconds* resident_age = nullptr);
    std::optional<response> get_copy(std::string_view key, std::chrono::seconds* resident_age = nullptr);
    void put(std::string key, response value, std::chrono::seconds ttl);
    void clear();
    [[nodiscard]] cache_statistics statistics() const;
};
```

线程安全的不可变响应缓存，按字符串键存取，分为若干独立加锁的分片，以 CLOCK 算法淘汰。它存储交给它的任何内容；上面的 HTTP 规则属于使用它的服务。

| 成员 | 作用 |
|:--|:--|
| `get(key, resident_age)` | 存储的快照；缺失或过期时为空指针。给出 `resident_age` 时写入自插入以来的时长 |
| `get_copy(key, resident_age)` | 存储响应的一份拥有数据的副本，或为空。在运行中的 context 上，它会记住为该缓存返回的最后一个快照，因此同一键的重复命中可以跳过分片锁和共享引用计数。在 context 外则复制 `get` 的结果 |
| `put(key, value, ttl)` | 以 `min(ttl, options.ttl)` 存储 `value`。缓存禁用、`ttl` 不为正、正文大于 `max_object_bytes` 或条目大于所在分片的预算时忽略。替换同键条目；必要时淘汰 |
| `clear()` | 删除所有条目，统计保留。已经返回的响应仍然有效 |
| `statistics()` | 当前计数器 |

`clear` 或被替换的条目在下一次查找时生效；与 `clear` 竞争的一次未命中可能在之后再次存入其结果，同一键的并发未命中也不会合并。`get_copy` 为每个缓存、每个 context 保留一个快照，它可能在被淘汰后继续存在，直到该 context 上的下一次查找或 context 销毁；请一次性创建缓存及带缓存的服务，而不是每个请求创建一次。

### `cache_options`

```cpp
struct cache_options
{
    std::size_t max_bytes = 0;
    std::size_t max_entries = 1024;
    std::size_t max_object_bytes = std::size_t{1024} * 1024;
    std::chrono::seconds ttl{60};
    std::size_t shards = 16;
};
```

| 成员 | 含义 |
|:--|:--|
| `max_bytes` | 已存条目的预算，计入正文、键和头部字段。0 表示禁用缓存 |
| `max_entries` | 条目数；至少为 1 |
| `max_object_bytes` | 存储的最大正文 |
| `ttl` | 条目的最长生存时间；不得为负 |
| `shards` | 分片数，1 到 256（最多使用 `max_entries` 个）。预算和条目在分片间平均分配，因此键分布不均时总预算可能有一部分用不上 |

`max_bytes` 不是进程内存上限：slot 存储、分配器开销以及仍被进行中的响应或 `get_copy` 持有的快照都在此之外。

### `cache_statistics`

```cpp
struct cache_statistics
{
    std::uint64_t hits = 0, misses = 0, evictions = 0;
    std::size_t entries = 0, bytes = 0;
};
```

命中数（包括所有 context 上的 `get_copy` 命中，无论 context 是否已销毁）、未命中数、淘汰数、当前条目数和计入的字节数。计数器逐分片累加，因此在有流量时取得的样本不是一个原子快照。

## 错误

| 情况 | 结果 |
|:--|:--|
| 前缀不以 `/` 开头，或含 `?`、`#`、`\`、`%`、空格或控制字符 | 构造函数抛出 `std::invalid_argument` |
| `index` 或 `cache_control` 无效 | `static_files` 构造函数抛出 `std::invalid_argument` |
| 静态目录无法打开 | `static_files` 构造函数抛出 `std::system_error` |
| 没有上游、`max_connections` 为 0、`max_idle_connections > max_connections`、`timeout` 不为正、`idle_timeout` 或 `fail_timeout` 为负 | `reverse_proxy` 构造函数抛出 `std::invalid_argument` |
| 上游 URL 无效、带查询串或片段、没有 TLS 支持却是 HTTPS、没有 DNS 支持却是主机名 | `reverse_proxy` 构造函数抛出 `std::invalid_argument` |
| `cache_options` 的 `shards` 为 0 或超过 256、`max_entries` 为 0 或 `ttl` 为负 | 构造函数抛出 `std::invalid_argument` |
| 静态请求：方法不对 / 路径不安全 / 文件不存在 / 文件过大 | 405 / 400 / 404 / 413 响应 |
| 代理：没有符合条件的上游 | 503 响应 |
| 代理：连接、TLS 或传输失败 | 502 响应 |
| 代理：上游响应格式错误 | 502 响应 |
| 代理：到达 `timeout` | 504 响应 |
| 代理：`CONNECT` 或 `Upgrade` | 501 响应 |
| 下游请求被取消 | 以 `std::system_error` 传播 |

截止时间只能打断处于等待中的操作，无法停止同步工作。

## 线程与并发

服务和缓存可以被任意数量的 worker context 共享。缓存内部已经同步；`clear_cache`、`cache_stats` 以及 `response_cache` 的所有成员都可以从任意线程调用。代理的连接池、活动计数和轮询位置位于各个 context 中，从不共享，因此各项上限和负载均衡都按 worker 计。静态文件读取在服务该请求的 context 上同步进行。

## 生命周期与所有权

`static_files` 和 `reverse_proxy` 是指向共享状态的可复制句柄。`operator()` 返回的 task 持有服务的一个引用和请求，因此在请求进行中可以销毁服务对象。用过某个代理、或对某个缓存调用过 `get_copy` 的 context 会保留相应状态直到该 context 销毁：请配置一组固定的服务，而不是每个请求创建一个。`get` 返回的快照在被持有期间始终有效，与缓存无关。

## 限制与默认值

| 项目 | 默认值 |
|:--|:--|
| `static_options::index` | `index.html` |
| `static_options::max_file_bytes` | 8 MiB |
| `static_options::allow_dotfiles` | `false` |
| `static_options::cache_control` | `no-cache` |
| `proxy_options::balance` | `round_robin` |
| `proxy_options::strip_prefix` | `true` |
| `proxy_options::max_connections` | 每个上游、每个 context 64 |
| `proxy_options::max_idle_connections` | 每个上游、每个 context 16 |
| `proxy_options::max_response_bytes` | 8 MiB |
| `proxy_options::timeout` | 30 秒 |
| `proxy_options::idle_timeout` | 30 秒 |
| `proxy_options::fail_timeout` | 1 秒 |
| `cache_options::max_bytes` | 0（缓存禁用） |
| `cache_options::max_entries` | 1024 |
| `cache_options::max_object_bytes` | 1 MiB |
| `cache_options::ttl` | 60 秒 |
| `cache_options::shards` | 16 |
| 可缓存的代理目标长度 | 4096 字节 |

静态 ETag 使用的哈希改编自 [xxHash 0.8.3](https://github.com/Cyan4973/xxHash/tree/v0.8.3)（BSD-2-Clause），其版权与许可保留在 `src/gateway/content_hash.hpp` 中。它不引入任何依赖。

## 参见

- [HTTP/1 应用](../http1/app.zh.md)、[路由](../http/router.zh.md)、[HTTP/1 服务器](../http1/server.zh.md)
- [网关性能](../../performance/http-gateway.zh.md)
- 教程：[网关](../../tutorial/14-gateway.zh.md)、[worker 与线程](../../tutorial/08-workers-and-threads.zh.md)
