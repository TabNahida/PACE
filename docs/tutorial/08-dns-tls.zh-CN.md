# 8. 把域名解析和 TLS 身份验证接起来

[English](08-dns-tls.md) · [上一章：UDP](07-udp.zh-CN.md) · [下一章：WebSocket](09-websocket.zh-CN.md)

网络地址回答“连接到哪里”，TLS peer name 回答“验证对方是谁”。
解析得到 IP 后仍应保留原域名，不能把两者混为一谈。

## 第一步：先完成一个不依赖外网的解析

配置 `--dns=y`，应用依赖 `pace_dns`，将 main.cpp 写成：

```cpp
#include <pace/dns.hpp>
#include <pace/run.hpp>
#include <iostream>

int main() {
    try {
        pace::run([]() -> pace::task<void> {
            const auto result = co_await pace::dns::resolve("127.0.0.1");
            for (const auto& address : result.addresses)
                std::cout << address.text << '\n';
        });
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

应输出 `127.0.0.1`。数字地址解析不需要外部 nameserver，适合先验证程序结构。
随后换成实际服务域名；结果可能包含 IPv4 和 IPv6，选择一个数字地址再和端口组成 endpoint。
解析不会自动连接，也不等于自动尝试所有地址；便捷函数会持有 resolver 到解析完成。
配套命令 `xmake run tutorial_dns example.com` 需要可用的外部 DNS。

## 第二步：连接之前先配置证书与信任

启用 `--tls=y`，依赖 `pace_tls`；HTTPS 应用仍依赖 `pace_http1`。
在 PACE 仓库根的本地练习可使用以下公开测试证书：

```cpp
pace::tls::context server_config(pace::tls::role::server), client_config;
server_config.use_certificate_chain("tests/certs/localhost.pem", "tests/certs/localhost-key.pem");
client_config.load_verify_file("tests/certs/ca.pem");
server_config.set_alpn_protocols({"pace-demo"});
client_config.set_alpn_protocols({"pace-demo"});
```

部署时换成自己的证书、私钥和正确的信任配置，不能部署公开测试私钥。
调用前完成配置，并让自定义配置活到 helper 的 await 完成；返回的 stream 会继续持有配置状态。

## 第三步：在 TCP 连接任务里完成 TLS 握手

把接受到的 TCP stream 移入 TLS stream，再握手：

```cpp
auto connection = co_await pace::tls::accept(std::move(accepted_tcp_stream), server_config);
co_await pace::write(connection, "Hello over verified TLS!\n");
co_await connection.shutdown();
```

客户端明确写出网络端点和验证身份：

```cpp
auto connection = co_await pace::tls::connect(client_config, endpoint, "localhost");
auto message = co_await pace::read_exact(connection, 25);
std::cout << message;
co_await connection.shutdown();
```

这两个 helper 都在握手成功后才返回。`pace::read_exact`、`pace::write` 与 TCP
使用相同写法，本例问候语恰好 25 字节。endpoint 可以是 `127.0.0.1`，证书身份则验证 `localhost`。
实际域名解析后，把原域名保留为 peer name。读写形式和 TCP 类似，但 TLS 的认证关闭
有额外意义；底层 TCP 突然 EOF 不应被当成一个正常完整的 TLS 结尾。

## 第四步：验证成功路径，再主动制造一次失败

在仓库根运行：

```sh
xmake f --tls=y --dns=y --examples=y -y
xmake -y tutorial_tls tutorial_dns
xmake run tutorial_tls
xmake run tutorial_dns
```

[TLS 配套程序](../../examples/tutorial_tls.cpp)把配置和 listener 放在任务外，
检查 ALPN、完整正文，并等两端完成关闭再 stop。
在你自己的副本中把 peer name 换成不相干域名，确认握手被拒绝；
这正是身份验证的效果，不应通过关闭验证来消除它。

HTTPS 可以直接复用这些配置，见[第四章](04-http.zh-CN.md#启用-https)。
[DNS 参考](../reference/dns.md) · [TLS 参考](../reference/tls.md)

使用系统信任且不需要自定义 ALPN 时，可省略客户端配置：
`co_await pace::tls::connect(endpoint, hostname)`，仍要保留原始 DNS 名用于证书验证。
在事件循环外只做一次解析，可以直接写
`auto addresses = pace::run(pace::dns::resolve("127.0.0.1"));`。

[便捷接口契约](../reference/component-conveniences.zh-CN.md)和
[配置复用、ALPN 调优](../reference/protocol-tuning.zh-CN.md#dns-与-tls)介绍后续细节。
本章新增 helper 需要当前 checkout。
