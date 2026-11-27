<!-- source-hash: 780d1d2f548b -->
# 1. 安装 PACE，构建第一个程序

[English](01-install-and-first-program.md) · [学习路线](README.zh.md)

本章建立一个属于你自己的项目，从源码构建 PACE，写一个小型 HTTP 服务器，并用 curl 验证它。后续各章都在这个项目里继续添加程序。

## 安装 xmake 和编译器

构建 PACE 需要 [xmake](https://xmake.io/) 2.9.8 或更新版本，以及一个 C++20 编译器：GCC、Clang 或 MSVC。CI 在 Linux、macOS、Windows 和 64 位 FreeBSD 上构建；具体配置见[平台支持](../reference/platform-support.zh.md)。

- Linux 和 macOS：按 xmake 官网的说明安装 xmake，编译器用系统包管理器或 Xcode 命令行工具提供的即可。
- Windows：安装 xmake 和带 C++ 工作负载的 Visual Studio，然后在具备 Visual Studio C++ 环境的 shell 中操作。
- FreeBSD：从上游安装 **xmake-io**。FreeBSD 中名为 `xmake` 的包是另一个工具。[CI 工作流](../../.github/workflows/ci.yml)里有固定版本的源码安装示例。

用 `xmake --version` 检查安装结果。此外还需要 git，以及用来访问下面服务器的 curl。

## 创建项目

按以下结构建立目录：

```text
hello/
  xmake.lua
  main.cpp
  third_party/PACE/
```

在 `hello` 目录下，把 PACE 源码放到 `third_party/PACE`：

```sh
git clone https://github.com/TabNahida/PACE.git third_party/PACE
```

编写 `xmake.lua`：

```lua
set_project("hello")
set_languages("cxx20")
add_rules("mode.debug", "mode.release")
includes("third_party/PACE")

target("hello")
    set_kind("binary")
    add_files("main.cpp")
    add_deps("pace_http1")
```

`includes` 加载 PACE 的构建描述，它的选项和库目标由此成为你项目的一部分。`add_deps("pace_http1")` 链接 HTTP/1 模块，并通过它带上其依赖的模块（runtime、net 和共享的 HTTP 类型），同时提供头文件路径、特性宏定义和平台库。不要把 PACE 的 `.cpp` 文件复制进你的目标，也不要手写链接顺序。

## 编写第一个程序

编写 `main.cpp`：

```cpp
#include <pace/http1.hpp>
#include <iostream>

int main()
{
    try
    {
        pace::http::app app;
        app.get("/health", [] { return pace::http::text("ok\n"); });
        app.post("/echo", [](pace::http::request request) { return pace::http::text(std::move(request.body)); });
        std::cout << "Listening on http://127.0.0.1:8080\n";
        app.run(8080);
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`http::app` 保存路由。`get` 和 `post` 为某个方法和某个精确路径注册处理函数。`http::text` 生成一个纯文本响应，响应拥有自己的 body；`/echo` 的处理函数把收到的 body 原样返回。`app.run(8080)` 监听 `127.0.0.1:8080`，在当前线程上提供服务，直到进程结束。这个程序里没有协程；[第 3 章](03-http-server.zh.md)会说明协程从哪里开始出现。

## 构建并运行

在 `hello` 目录下：

```sh
xmake f -m release --tests=n --examples=n -y
xmake -y hello
xmake run hello
```

`xmake f` 用来配置项目；`--tests=n --examples=n` 让 PACE 自带的测试和示例不进入你的构建。在第二个终端中：

```sh
curl -i http://127.0.0.1:8080/health
curl --data-binary hello http://127.0.0.1:8080/echo
```

预期先得到状态 200 和 `ok`，然后是 `hello`。用 Ctrl-C 停止服务器。保留这个项目：下一章会用你自己写的程序向这个服务器发请求。

如果找不到 PACE 的头文件，检查 `includes` 路径和 `add_deps` 一行。如果 xmake 报告 `pace_tls` 之类的目标不存在，请打开对应的构建选项（见下一节）后重新配置。如果 curl 连不上，说明服务器没有运行，或者 8080 端口已被其他程序占用；后一种情况下服务器会报告绑定错误。

## 依赖需要的模块

每个模块都是单独的库目标。程序用到哪个就依赖哪个，它所依赖的模块会自动带上。

| 程序用到 | 构建选项 | 目标 |
|:--|:--|:--|
| 任务、定时器、运行时 | 始终构建 | `pace_runtime` |
| TCP 和 UDP | 始终构建 | `pace_net` |
| HTTP/1 客户端和服务器、`http::app`、静态文件与反向代理 | `--http1=y`（默认） | `pace_http1` |
| WebSocket | `--websocket=y`（默认） | `pace_websocket` |
| TLS | `--tls=y` | `pace_tls` |
| DNS | `--dns=y` | `pace_dns` |
| QUIC | `--quic=y` | `pace_quic` |
| HTTP/2 | `--http2=y`（HTTPS 另加 `--tls=y`） | `pace_http2` |
| HTTP/3 | `--http3=y`（同时启用 QUIC） | `pace_http3` |
| 本次构建启用的全部模块 | | `pace` |

修改 `add_deps` 后要重新配置再构建。启用某个模块的选项，在适用时也会把它接入 HTTP/1：`--dns=y` 后 URL 客户端可以使用主机名，`--tls=y` 后可以使用 `https://`。可选模块第一次配置时，xmake 会下载它们的第三方库（OpenSSL、c-ares、nghttp2、ngtcp2、nghttp3）。QUIC 和 HTTP/3 需要 POSIX 平台。`--http1=n --websocket=n` 得到只含运行时、TCP/UDP 和共享 HTTP 类型的构建，不依赖任何可选库。

[构建选项](../reference/build-options.zh.md)列出了所有开关、C++ 标准、sanitizer、QUIC 后端和 I/O 后端。

## 构建配套程序

大多数章节都会提到 PACE `examples/` 目录中的一个配套程序，它是本章练习的完整、可自检的版本。在 PACE 源码目录（`third_party/PACE`）中：

```sh
xmake f -m release --examples=y -y
xmake -y
xmake run tutorial_tasks
```

`tutorial_tasks` 输出 `Hello, PACE` 后退出。`xmake test` 运行 PACE 的正确性测试。介绍可选模块的章节会给出需要追加到 `xmake f` 的选项。

## C++ 命名模块（`import pace`）

除了头文件，PACE 也可以通过 C++20 命名模块使用。模块默认关闭；配置 `--modules=y`，并依赖 `pace_module` 而不是某个库目标：

```lua
target("hello_module")
    set_kind("binary")
    add_files("module_main.cpp")
    add_deps("pace_module")
```

`import pace;` 导入本次构建启用的所有模块，相当于 `#include <pace/pace.hpp>`。每个库模块还有自己的命名模块 `pace.<module>`：`pace.runtime`、`pace.net`、`pace.http`、`pace.http1`、`pace.gateway`、`pace.tls`、`pace.dns`、`pace.quic`、`pace.http2`、`pace.http3` 和 `pace.websocket`。每个命名模块只导出自己的名字，所以使用 TCP 的程序要同时导入 `pace.net` 和 `pace.runtime`。下面的程序只需要运行时：

```cpp
#include <coroutine>
#include <iostream>
import pace.runtime;

pace::task<int> answer()
{
    co_await pace::yield();
    co_return 42;
}

int main()
{
    std::cout << pace::run(answer) << '\n';
}
```

把 `import pace.runtime;` 换成 `import pace;` 即可导入全部内容。源文件用到的标准库头文件要放在 `import` 之前包含；不需要包含任何 PACE 头文件。模块不导出宏，xmake 仍然通过 `pace_module` 传递特性宏定义和链接依赖。

命名模块只是同一组静态库之上的导入接口：包含 PACE 头文件的翻译单元和导入 PACE 模块的翻译单元使用相同的类型，可以链接在一起。构建模块需要 Clang 并在 `PATH` 中提供与之匹配的 `clang-scan-deps`，或者使用 GCC 16 及以上版本；低于 16 的 GCC 会直接报错。MSVC 和 Apple Clang 的模块构建尚未验证。更换编译器、C++ 标准或模块选项后需要重新构建，也不要在不同工具链之间共享已构建的模块接口。已验证的工具链见[构建选项](../reference/build-options.zh.md)。

下一章：[2. 发送 HTTP 请求](02-http-client.zh.md)
