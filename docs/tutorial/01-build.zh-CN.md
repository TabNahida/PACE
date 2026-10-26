# 1. 从自己的 main.cpp 开始

[English / 完整构建选项](01-build.md) · [下一章：异步任务](02-tasks-timers.zh-CN.md)

这一章要做出你自己的程序，不是只运行仓库里写好的 example。
需要 xmake 2.9.8+ 和支持 C++20 的编译器。教程对应当前源码；
`pace::run` 和路由器是 v0.2.0 之后加入的接口。

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
    add_deps("pace_runtime")
```

`includes` 加载库的构建定义，`add_deps` 让应用得到头文件路径、编译定义和
系统链接依赖。无需复制库的 `.cpp` 文件，也不用手写静态库链接顺序。
我们先只使用 runtime，所以没有额外协议依赖。

## 第二步：写 main.cpp

```cpp
#include <pace/run.hpp>
#include <iostream>

pace::task<void> hello(pace::io_context&) {
    std::cout << "Hello, PACE\n";
    co_return;
}

int main() {
    try { pace::run(hello); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

暂时把 `task<void>` 理解为“稍后执行、不返回结果的任务”。
`run` 负责启动它，并在入口结束后退出。下一章再加真正的异步等待。

## 第三步：构建并核对结果

在 **hello 目录**执行：

```sh
xmake f -m release --tests=n --examples=n --http1=n --websocket=n -y
xmake -y my_application
xmake run my_application
```

应输出 `Hello, PACE`，然后正常结束。如果找不到头文件，检查 `includes`
路径和 `add_deps`；如果打印后不退出，检查是否改成了需要手动停止的 `io.run()`。

接下来的 TCP 章节把依赖换成 `pace_net`；HTTP 章节换成 `pace_http1`，
并重新配置 `--http1=y`。TLS、DNS、HTTP/2、QUIC、HTTP/3 要分别启用：

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
不要误用系统中同名的另一种 xmake。详见[完整构建说明](01-build.md)。
