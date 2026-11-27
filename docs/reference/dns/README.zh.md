<!-- source-hash: 2e344c94c92a -->
# DNS（`pace/dns.hpp`）

[English](README.md) · [参考手册](../README.zh.md)

## 概述

DNS 模块在 PACE 反应器上异步地把主机名解析为数字 IPv4 和 IPv6 地址，底层使用 [c-ares](https://c-ares.org/)，不需要
解析线程。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/dns.hpp>` | `pace_dns` | `--dns=y` |

`<pace/dns.hpp>` 包含 `<pace/dns/resolver.hpp>`。该构建选项定义 `PACE_HAS_DNS` 并链接 c-ares；关闭时头文件不声明
任何内容，TCP 和 UDP 模块也从不依赖解析器。启用 `--modules=y` 时，这些名字由 `import pace.dns;` 导出。仓库中的示例
是 `examples/tutorial_dns.cpp`：`xmake run tutorial_dns` 解析 `127.0.0.1`，不需要网络；
`xmake run tutorial_dns example.com` 使用系统配置的域名服务器。

## 概要

```cpp
#include <pace/dns.hpp>
#include <pace/net.hpp>
#include <pace/runtime.hpp>

#include <iostream>

pace::task<void> lookup(pace::io_context& io)
{
    const auto answer = co_await pace::dns::resolve(io, "localhost", pace::dns::family::any);
    for (const auto& address : answer.addresses)
    {
        std::cout << (address.type == pace::dns::family::ipv4 ? "IPv4 " : "IPv6 ") << address.text << '\n';
    }
    if (!answer.addresses.empty())
    {
        pace::tcp::endpoint server{answer.addresses.front().text, 80};
        std::cout << "would connect to " << server.address << '\n';
    }
}

int main()
{
    pace::run(lookup);
}
```

## 类型

### `family`

```cpp
enum class family
{
    any,
    ipv4,
    ipv6
};
```

请求的地址族：`any` 查询 A 和 AAAA 记录，`ipv4` 只查 A，`ipv6` 只查 AAAA。结果中的每个地址是 `ipv4` 或 `ipv6`，
从不为 `any`。

### `address`

```cpp
struct address
{
    family type = family::ipv4;
    std::string text;
};
```

一个解析得到的地址：`type` 是它的地址族，`text` 是它的数字文本，可以直接用于
[`tcp::endpoint`](../net/tcp.zh.md) 或 [`udp::endpoint`](../net/udp.zh.md)。

### `result`

```cpp
struct result
{
    std::string canonical_name;
    std::vector<address> addresses;
};
```

`canonical_name` 是查询报告的规范名（经过 CNAME 之后），`addresses` 是去重后的地址，顺序与 c-ares 返回的相同（不做
RFC 6724 排序）。成功的查询总是至少有一个地址。选择哪个地址、依次尝试多个地址、以及 IPv4 与 IPv6 的连接竞速，都由
应用负责。

### `options`

```cpp
struct options
{
    std::chrono::milliseconds timeout{5000};
    unsigned tries = 2;
    std::string servers;
};
```

| 成员 | 含义 |
|:--|:--|
| `timeout` | 整个查询（包括重试）的时间上限。必须为正，且不超过 24 小时 |
| `tries` | 每个域名服务器的尝试次数（c-ares 的 `tries`），1 到 10。每次尝试最多等待 1 s（`timeout` 更短时取 `timeout`） |
| `servers` | 为空时使用系统配置。否则是以逗号分隔的数字服务器地址，可带端口，例如 `127.0.0.1:5353,[::1]:5353`；不能是主机名 |

无效的选项由 `resolver` 构造函数抛出 `std::invalid_argument`。搜索域扩展已禁用：请传入要解析的完整名字。hosts 文件
仍会被查询。

### `resolver`

```cpp
class resolver
{
public:
    explicit resolver(io_context& io, options configuration = {});
    ~resolver();
    resolver(const resolver&) = delete;
    resolver& operator=(const resolver&) = delete;
    task<result> resolve(std::string hostname, family requested = family::any);
    void cancel() noexcept;
    io_context& context() const noexcept;
};
```

绑定到一个 `io_context`、同一时间只执行一个查询的解析器。不可复制，也不可移动。

- `resolve(hostname, requested)` 查询 `hostname` 并返回其地址。任务拥有这个名字。数字地址和 hosts 文件中的条目
  不发送 DNS 报文即可完成。开始查询时会同步读取系统解析器配置和 hosts 配置；之后的网络等待和重试都在反应器的就绪
  通知和定时器上进行。DNS 报文解析、CNAME 链以及从 UDP 回退到 TCP 都由 c-ares 处理。主机名必须是 1 到 253 字节，
  不含空格或控制字符（否则 `std::invalid_argument`）。
- `cancel()` 取消正在进行的查询（如果有），该查询随后抛出 `operation_canceled`。尚未开始的查询不受影响。
- `context()` 返回所属上下文。

查询失败、超时或被取消之后，解析器仍可用于下一次查询。

## 函数

### `resolve`

```cpp
task<result> resolve(io_context& io, std::string hostname, family requested = family::any, options configuration = {});
task<result> resolve(std::string hostname, family requested = family::any, options configuration = {});
```

一次性查询：任务在完成前拥有一个 `resolver`、名字和选项，因此可以与其他 `resolve` 调用并发执行。不带 `io` 的重载
使用运行该任务的上下文，在任务开始时读取；在运行中的上下文之外，请把它交给 `pace::run`，例如
`pace::run(pace::dns::resolve("127.0.0.1"))`。需要 `cancel()` 时请使用显式的 `resolver`。`resolve` 不选择端口、不
建立连接，也不尝试多个地址。

### `error_category`

```cpp
const std::error_category& error_category() noexcept;
```

DNS 失败所用的错误类别（`name()` 为 `"pace.dns"`）。其取值是 c-ares 状态码（`ARES_ENOTFOUND`、`ARES_ENODATA`、
`ARES_ESERVFAIL`、`ARES_EREFUSED` 等），`message()` 返回 c-ares 的描述。

## 错误

| 情形 | 异常 |
|:--|:--|
| 无效的 `options` | `resolver` 构造函数抛出 `std::invalid_argument` |
| 主机名为空、过长或格式错误，`family` 取值无效 | `std::invalid_argument` |
| 同一解析器上已有查询时再次 `resolve` | `std::logic_error` |
| 在解析器所属上下文之外执行 `resolve` | `std::logic_error` |
| 名字不存在、没有所请求地址族的地址、服务器失败、拒绝、应答格式错误 | `dns::error_category()` 中带 c-ares 状态码的 `std::system_error` |
| `cancel()`、解析器被销毁、上下文停止，或等待方任务被取消 | `std::system_error`（`std::errc::operation_canceled`） |
| `options::timeout` 或等待方任务的截止时间到期 | `std::system_error`（`std::errc::timed_out`） |

名字存在但没有所请求地址族的地址时，报告 `ARES_ENODATA`。

## 线程与并发

- `resolve` 必须在解析器所属的上下文上执行。`cancel()` 和析构函数必须在该上下文上执行，或在它未运行时执行。
- 一个解析器同时只允许一个查询。并发查询请使用多个 `resolver` 对象，或者使用各自拥有解析器的自由函数 `resolve`。

## 生命周期与所有权

- `io_context` 的寿命必须长于所有查询任务，包括已销毁的解析器发起的任务。
- `resolve` 拥有它的主机名和查询状态；该状态由任务共享，即使 `resolver` 被销毁也仍然有效。销毁解析器会取消正在进行
  的查询。
- 查询结束后，内部的定时器协程可能还会持有已完成的查询状态最多 100 ms。这段时间内它不持有任何 DNS 套接字。

## 限制与默认值

| 项目 | 默认值或上限 |
|:--|:--|
| `options::timeout` | 5 s；允许范围 (0, 24 h] |
| `options::tries` | 2；允许 1–10 |
| 每次尝试的等待时间 | min(1 s, `timeout`) |
| `options::servers` | 空（使用系统配置） |
| 主机名长度 | 1–253 字节 |
| 每个 `resolver` 同时进行的查询 | 1 |
| 搜索域扩展 | 禁用 |
| 查询定时器保留已完成状态的时间 | 最多 100 ms |

## 参见

- 连接解析结果用 [TCP](../net/tcp.zh.md)、[UDP](../net/udp.zh.md)
- [TLS](../tls/README.zh.md)：对端名字与数字地址分开传入
- [HTTP 客户端](../http/clients.zh.md)，它们用本模块解析 URL 中的主机名
- 截止时间见[结构化并发](../runtime/structured-concurrency.zh.md)
- 教程：[TLS 与 DNS](../../tutorial/10-tls-and-dns.zh.md)
- c-ares：[事件循环集成](https://c-ares.org/docs.html)、[`ares_getaddrinfo`](https://c-ares.org/docs/ares_getaddrinfo.html)
