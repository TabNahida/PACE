# 1. Install PACE and build your first program

[中文](01-install-and-first-program.zh.md) · [Learning path](README.md)

In this chapter you set up a project of your own that builds PACE from source, write a small HTTP server and
check it with curl. The following chapters add programs to the same project.

## Install xmake and a compiler

PACE builds with [xmake](https://xmake.io/) 2.9.8 or newer and a C++20 compiler: GCC, Clang or MSVC. CI builds it
on Linux, macOS, Windows and 64-bit FreeBSD; [platform support](../reference/platform-support.md) lists the
exact configurations.

- Linux and macOS: install xmake as described on its site, and a compiler from your package manager or the Xcode
  command-line tools.
- Windows: install xmake and Visual Studio with the C++ workload, then work in a shell that has the Visual Studio
  C++ environment.
- FreeBSD: install **xmake-io** from upstream. The FreeBSD package named `xmake` is a different tool. The
  [CI workflow](../../.github/workflows/ci.yml) shows a pinned source install.

Check the installation with `xmake --version`. You also need git, and curl to try the server below.

## Create the project

Make a directory with this layout:

```text
hello/
  xmake.lua
  main.cpp
  third_party/PACE/
```

From `hello`, put the PACE sources into `third_party/PACE`:

```sh
git clone https://github.com/TabNahida/PACE.git third_party/PACE
```

Write `xmake.lua`:

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

`includes` loads PACE's build description, so its options and library targets become part of your project.
`add_deps("pace_http1")` links the HTTP/1 module and, through it, the modules it depends on (runtime, net and the
shared HTTP types). It also supplies the include path, the feature definitions and the platform libraries. Do not
copy PACE's `.cpp` files into your target or write a link order by hand.

## Write the first program

Write `main.cpp`:

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

`http::app` holds the routes. `get` and `post` register a handler for a method and an exact path. `http::text`
makes a plain-text response that owns its body; the `/echo` handler returns the body it received.
`app.run(8080)` listens on `127.0.0.1:8080` and serves on this thread until the process ends. There is no
coroutine in this program; [chapter 3](03-http-server.md) shows where coroutines come in.

## Build and run

From `hello`:

```sh
xmake f -m release --tests=n --examples=n -y
xmake -y hello
xmake run hello
```

`xmake f` configures the project; `--tests=n --examples=n` keeps PACE's own tests and examples out of your build.
In a second terminal:

```sh
curl -i http://127.0.0.1:8080/health
curl --data-binary hello http://127.0.0.1:8080/echo
```

Expect status 200 with `ok`, then `hello`. Stop the server with Ctrl-C. Keep this project: the next chapter sends
requests to this server from a program of your own.

If a PACE header cannot be found, check the `includes` path and the `add_deps` line. If xmake reports an unknown
target such as `pace_tls`, enable its build option (next section) and configure again. If curl cannot connect, the
server is not running or another program already uses port 8080; the server then reports the bind error.

## Depend on the module you need

Each module is its own library target. Depend on the one your program uses; its dependencies come with it.

| Your program uses | Build option | Target |
|:--|:--|:--|
| Tasks, timers, runtime | always built | `pace_runtime` |
| TCP and UDP | always built | `pace_net` |
| HTTP/1 clients and servers, `http::app`, static files and proxy | `--http1=y` (default) | `pace_http1` |
| WebSocket | `--websocket=y` (default) | `pace_websocket` |
| TLS | `--tls=y` | `pace_tls` |
| DNS | `--dns=y` | `pace_dns` |
| QUIC | `--quic=y` | `pace_quic` |
| HTTP/2 | `--http2=y` (add `--tls=y` for HTTPS) | `pace_http2` |
| HTTP/3 | `--http3=y` (enables QUIC) | `pace_http3` |
| Everything enabled in this build | | `pace` |

Change `add_deps` and configure again before building. An option that enables a module also adds it to HTTP/1
where it applies: with `--dns=y` the URL clients accept hostnames, and with `--tls=y` they accept `https://`.
Optional modules download their third-party libraries (OpenSSL, c-ares, nghttp2, ngtcp2, nghttp3) through xmake
the first time you configure them. QUIC and HTTP/3 need a POSIX platform. `--http1=n --websocket=n` gives a build
with only the runtime, TCP/UDP and the shared HTTP types, without any optional dependency.

[Build options](../reference/build-options.md) covers every flag, the C++ standard, sanitizers, the QUIC backend
and the I/O backends.

## Build the companion programs

Most chapters name a companion program in PACE's `examples/` directory. It is a complete, self-checking version of
the chapter's exercise. From the PACE checkout (`third_party/PACE`):

```sh
xmake f -m release --examples=y -y
xmake -y
xmake run tutorial_tasks
```

`tutorial_tasks` prints `Hello, PACE` and exits. `xmake test` runs PACE's correctness tests. Chapters about
optional modules give the extra options to add to `xmake f`.

## C++ named module (`import pace`)

PACE can also be used through C++20 named modules instead of headers. Modules are off by default; configure
`--modules=y` and depend on `pace_module` instead of a library target:

```lua
target("hello_module")
    set_kind("binary")
    add_files("module_main.cpp")
    add_deps("pace_module")
```

`import pace;` brings in every module enabled in the build, like `#include <pace/pace.hpp>`. Each library module
also has its own named module, `pace.<module>`: `pace.runtime`, `pace.net`, `pace.http`, `pace.http1`,
`pace.gateway`, `pace.tls`, `pace.dns`, `pace.quic`, `pace.http2`, `pace.http3` and `pace.websocket`. Each exports
only its own names, so a program that uses TCP imports `pace.net` and `pace.runtime`. This program needs only the
runtime:

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

Replace `import pace.runtime;` with `import pace;` to import everything. Include the standard headers a source file
uses before the `import`; it needs no PACE header. Modules do not export macros, and xmake still passes the
feature definitions and link dependencies through `pace_module`.

The modules are an import interface over the same static libraries: a translation unit that includes PACE headers
and one that imports PACE modules use the same types and link together. Build the module with Clang and a matching
`clang-scan-deps` on `PATH`, or with GCC 16 or newer; GCC before 16 is rejected with an error. MSVC and Apple Clang
module builds are not validated. Rebuild after changing the compiler, the C++ standard or a module option, and do
not share built module interfaces between toolchains. [Build options](../reference/build-options.md) lists the
validated toolchains.

Next: [2. Make HTTP requests](02-http-client.md)
