<!-- source-hash: 7d5a0b189fe4 -->
# 10. TLS 与 DNS

[English](10-tls-and-dns.md) · [学习路线](README.zh.md) · [上一章：有序关闭](09-graceful-shutdown.zh.md)

网络地址告诉你连到哪里，TLS 对端名告诉你要校验哪个身份。两者要分开：解析出的 IP 地址并不能代替原来的主机名。

这两个模块都是可选的。开始之前，用它们重新配置 `hello` 项目；xmake 第一次会下载 c-ares 和 OpenSSL：

```sh
xmake f -m release --tests=n --examples=n --dns=y --tls=y -y
```

## 从确定性的解析开始

把这个完整程序保存为 `resolve.cpp`，并添加一个依赖 `pace_dns` 的 `resolve` 目标：

```cpp
#include <pace/dns.hpp>
#include <pace/runtime.hpp>
#include <iostream>

int main()
{
    try
    {
        pace::run(
            []() -> pace::task<void>
            {
                const auto result = co_await pace::dns::resolve("127.0.0.1");
                for (const auto& address : result.addresses)
                {
                    std::cout << address.text << '\n';
                }
            }
        );
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

预期输出 `127.0.0.1`。数字形式的名字无需询问名字服务器即可解析。现在把它换成你的服务的主机名。结果可能同时包含 IPv4 和 IPv6 地址：选一个，再加上服务端口构造 TCP 端点。解析既不会建立连接，也不会替你逐个尝试地址。`dns::resolve` 在完成之前拥有自己的解析器，默认五秒后放弃。

配套程序 `xmake run tutorial_dns` 解析 `127.0.0.1`，并输出每个地址及其地址族；`xmake run tutorial_dns example.com` 解析真实的名字，需要可用的外部 DNS。先在 PACE 源码目录中以 `--dns=y --examples=y` 配置。

## 打开 TLS 流之前配置信任

链接 `pace_tls`（如果做 HTTPS 则链接 `pace_http1`，启用 TLS 时它会一并带上 TLS）。本地练习使用 PACE 源码中公开的测试 CA 和 localhost 证书：

```cpp
pace::tls::context server_config(pace::tls::role::server), client_config;
server_config.use_certificate_chain("tests/certs/localhost.pem", "tests/certs/localhost-key.pem");
client_config.load_verify_file("tests/certs/ca.pem");
server_config.set_alpn_protocols({"pace-demo"});
client_config.set_alpn_protocols({"pace-demo"});
```

这些路径相对于 PACE 源码目录。`xmake run` 在构建目录中启动程序，所以请在 `hello` 目录下用 `xmake run -w third_party/PACE <target>` 运行，或者改用绝对路径。部署时请使用你自己的证书链、私钥和信任根。不要为了让示例连通而关闭校验。context 在首次使用之前就要配置完整：某个流使用过它之后，再调用设置函数会抛出异常。在借用 context 的操作完成之前保持它存活；流本身共享其状态的所有权。

## 升级 TCP 连接任务

在 TCP 服务器处理单个连接的协程中，把普通流换成 TLS 流：

```cpp
auto connection = co_await pace::tls::accept(std::move(accepted_tcp_stream), server_config);
co_await pace::write(connection, "Hello over verified TLS!\n");
co_await connection.shutdown();
```

在客户端，指明握手必须校验的身份：

```cpp
auto connection = co_await pace::tls::connect(endpoint, "localhost", {.tls = &client_config});
auto message = co_await pace::read_exact(connection, 25);
std::cout << message;
co_await connection.shutdown();
```

两个调用都只在握手成功后返回。`pace::read_exact` 和 `pace::write` 在 TLS 流上的用法与 TCP 相同；这里的问候语恰好 25 字节。端点可以是 `127.0.0.1`，而证书按 `localhost` 校验。对于真实主机，把你解析时用的主机名保留为对端名。TLS 用经过认证的关闭通知结束一个流：裸的 TCP 流结束并不代表 TLS 消息成功结束。

## 运行两端，再试一次失败

在 PACE 源码目录中：

```sh
xmake f -m release --tls=y --dns=y --examples=y -y
xmake -y tutorial_tls tutorial_dns
xmake run tutorial_tls
xmake run tutorial_dns
```

配套程序 [`tutorial_tls.cpp`](../../examples/tutorial_tls.cpp) 把两个配置和监听器都放在任务之外，检查协商出的 ALPN 协议，读取完整的问候语，确认其后紧跟对端的关闭通知，并等两端都完成关闭后才停止。它输出 `Hello over verified TLS!`。在你自己的副本中，把期望的对端名改成一个无关的主机，确认握手失败。不要为了掩盖这个失败而关闭校验。

对于证书受公开信任、又不需要自定义 ALPN 的服务，客户端无需任何配置：`co_await pace::tls::connect(endpoint, hostname)` 用 OpenSSL 的默认信任库校验（信任库为空时见[第 2 章](02-http-client.zh.md)）。把原始的 DNS 主机名作为要校验的名字。在没有运行上下文的地方做单次查询，可以用 `auto result = pace::run(pace::dns::resolve("127.0.0.1"));`。

对于 HTTP，给 HTTPS 重载传入一个加载了你的 CA 的客户端 context，例如[第 2 章](02-http-client.zh.md)中的 `pace::http::get(trust, url)`；不要设置 `pace-demo` 这个 ALPN，它只是这两个对端之间的约定。HTTPS 服务器见 [HTTP 实用配方](../guides/http-recipes.zh.md)。[DNS 参考](../reference/dns/README.zh.md)和 [TLS 参考](../reference/tls/README.zh.md)描述超时、信任、SNI 和所有权；[协议调优](../guides/protocol-tuning.zh.md)介绍如何复用配置和选择 ALPN。

下一章：[11. WebSocket](11-websocket.zh.md)
