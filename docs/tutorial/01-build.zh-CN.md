# 1. 从自己的 main.cpp 开始

[English](01-build.md) · [完整构建选项](../reference/build-configuration.md) · [下一章：异步任务](02-tasks-timers.zh-CN.md)

这一章要做出你自己的程序，不是只运行仓库里写好的 example。
需要 xmake 2.9.8+ 和支持 C++20 的编译器。教程需要 PACE 0.4.0 或更新版本；
下文使用 v0.4.0 源码 checkout。

## 第一步：准备三个文件位置

创建练习目录，把包含本教程的 PACE 源码放在 `third_party/PACE`：

```text
hello/
  xmake.lua
  main.cpp
  third_party/PACE/
```

在 **hello/xmake.lua** 写入：

```lua
set_project("hello")
set_languages("cxx20")
add_rules("mode.debug", "mode.release")
includes("third_party/PACE")

target("my_application")
    set_kind("binary")
    add_files("main.cpp")
    add_deps("pace_http1")
```

`includes` 加载库的构建定义，`add_deps` 让应用得到头文件路径、编译定义和
系统链接依赖。无需复制库的 `.cpp` 文件，也不用手写静态库链接顺序。
这里链接 `pace_http1`，它自动带上 TCP 和 runtime 依赖；数字 HTTP 地址不需要第三方协议库。

## 第二步：写 main.cpp

```cpp
#include <pace/http.hpp>
#include <iostream>

int main() {
    try {
        auto reply = pace::http::get("http://127.0.0.1:8080/health");
        std::cout << reply.status << " " << reply.body;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`get` 直接返回响应，不需要协程或 `run`。保持[快速入门的服务端](00-quick-start.zh-CN.md)
在另一个终端运行，再构建和运行本章客户端。

## 第三步：构建并核对结果

在 **hello 目录**执行：

```sh
xmake f -m release --tests=n --examples=n --http1=y --websocket=n -y
xmake -y my_application
xmake run my_application
```

应输出 `200 ok` 后退出。找不到头文件时检查 `includes` 路径和 `add_deps`；
连接失败时先确认本地服务端仍在运行，并检查 URL 端口。

接着按需阅读 HTTP、TCP 或 UDP 章节，需要异步组合时再读第二章。
TCP/UDP 只需把依赖换成 `pace_net`。TLS、DNS、HTTP/2、QUIC、HTTP/3 要分别启用：

| 功能 | 选项 | 应用依赖 |
|---|---|---|
| TCP / UDP | 无额外选项 | `pace_net` |
| HTTP/1 | `--http1=y` | `pace_http1` |
| WebSocket | `--websocket=y` | `pace_websocket` |
| TLS / HTTPS | `--tls=y` | `pace_tls` / `pace_http1` |
| DNS | `--dns=y` | `pace_dns` |
| HTTP/2 | `--http2=y`，HTTPS 另加 `--tls=y` | `pace_http2` |
| QUIC / HTTP/3 | `--quic=y` / `--http3=y` | `pace_quic` / `pace_http3` |

QUIC/HTTP3 当前需要 POSIX 平台。HTTP3 自动启用 QUIC；它自己的 TLS 1.3
不要求开启 TCP TLS 模块。首次启用可选协议时，xmake 可能需要下载依赖。

## 第四步：用配套练习核对理解

文中的 `tutorial_*` 命令在 **PACE 仓库根目录**执行，和 hello 应用是两套 target：

```sh
xmake f -m release --examples=y --tests=y -y
xmake -y tutorial_tasks
xmake run tutorial_tasks
```

先按 Markdown 自己写，再用配套程序核对结果。所有默认教程命令都是有结束点的
本地练习；带 `serve` 的 TCP/HTTP 示例会持续监听，等待 Ctrl-C。

配置会被 xmake 保留。切换协议选项后重新运行配置和构建；独立构建可使用
`XMAKE_CONFIGDIR`，仅改变输出目录 `-o` 并不能隔离配置。
Windows 需要可用的 Visual Studio C++ 工具链；FreeBSD 应安装 xmake-io 的工具，
不要误用系统中同名的另一种 xmake。详见[完整构建说明](../reference/build-configuration.md)。
