<!-- source-hash: cd6f3928939d -->
# 错误

[English](errors.md) · [参考](README.zh.md)

## 概述

PACE 用异常报告失败。等待一个任务时，会重新抛出该任务抛出的异常，因此错误像普通 C++ 异常一样沿 `co_await`
链传递。各模块页面列出各自操作的错误；本页说明它们共同使用的几类错误。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| 标准库 `<system_error>`、`<stdexcept>`；模块特有的类型见 `<pace/http.hpp>`、`<pace/websocket.hpp>`、`<pace/dns.hpp>` | — | — |

## 类型

| 异常 | 含义 | 常见原因 |
|:--|:--|:--|
| `std::system_error` | 操作在运行时失败 | 操作系统报告的套接字错误（`connection_reset`、`broken_pipe` 等）；取消；截止时间到期；TLS 握手或证书验证失败；DNS 查询失败 |
| `std::invalid_argument` | 参数格式不正确 | 无法解析的地址、端点或 URL；非法的头部名称或值；超出范围的限制值或选项 |
| `std::logic_error` | 对象当前的状态不允许这次调用 | 从上下文以外的线程发起操作；在只允许一个操作的地方发起第二个并发操作；握手之前就使用流；在正在运行的上下文中调用阻塞式客户端；嵌套调用 `pace::run` |
| `std::length_error` | 超出了配置的限制 | 消息体、头部块或流的数量超过对应的 `limits` 值 |
| `std::overflow_error` | 长度计算会溢出 | 长度接近 `std::size_t` 的上限 |
| `pace::http::protocol_error` | HTTP 对端违反了协议 | 客户端收到格式错误或被截断的响应；连接随之关闭 |
| `pace::websocket::protocol_error` | WebSocket 对端违反了协议 | `close_code()` 返回发给对端的关闭码 |
| `std::runtime_error` | 协议实现内部的其他失败 | HTTP/2 会话错误，例如对端发送 `GOAWAY` 或重置流 |
| `std::bad_alloc` | 内存分配失败 | 任何会分配内存的操作都可能抛出 |

`std::logic_error` 和 `std::invalid_argument` 说明程序本身有错：应当修正调用，而不是重试。`std::system_error`
和协议错误描述的是网络和对端的情况，在正确的程序里也会出现。

## 错误码

`std::system_error` 携带一个 `std::error_code`。应与 `std::errc` 的值比较，而不要去匹配错误消息：

| 错误码 | 何时出现 |
|:--|:--|
| `std::errc::operation_canceled` | 操作挂起期间，上下文被停止、取消令牌被触发，或套接字被关闭 |
| `std::errc::timed_out` | 截止时间到期：`blocking_options` 的截止时间、作用域的截止时间、HTTP 截止时间，或整个 DNS 查询的时限 |
| `std::errc::bad_file_descriptor` | 套接字或流已经关闭 |
| `std::errc::protocol_error` | TLS 握手、证书验证或记录层失败；消息中包含 OpenSSL 给出的原因 |
| `std::errc::not_supported`、`operation_not_supported`、`address_family_not_supported` | 平台或当前构建不具备该功能，例如操作系统不提供的选项 |
| `std::errc::message_size`、`no_buffer_space` | 数据报放不下，或操作系统无法将其排队 |
| 其他任何值 | 直接来自操作系统（`std::system_category()` 或 `std::generic_category()`） |

DNS 失败在 `pace::dns::error_category()` 中保留 c-ares 的状态码；DNS 查询被取消或整个查询超时，则改用通用的
`operation_canceled` 和 `timed_out`。

```cpp
try
{
    co_await stream.read_some(buffer);
}
catch (const std::system_error& error)
{
    if (error.code() == std::errc::operation_canceled)
    {
        co_return; // 正在关闭：属于预期情况
    }
    throw;
}
```

## 线程与并发

根任务或投递的函数中抛出、且未在其中捕获的异常会停止它所在的 `io_context`；`run()` 在上下文排空之后重新抛出
第一个这样的异常。`runtime` 中某个工作线程失败时，会停止其余工作线程，并在全部 join 之后重新抛出。上下文正在
停止时，根任务中未捕获的 `operation_canceled` 不视为失败。

## 参见

- [运行时](runtime/README.zh.md)：错误和取消如何在任务与上下文之间传播
- [结构化并发](runtime/structured-concurrency.zh.md)：取消令牌与截止时间
- [执行模型](../explanation/execution-model.zh.md#跨线程的错误)
