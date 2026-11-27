<!-- source-hash: e90c9e4ff36f -->
# 参与开发

[English](README.md) · [文档](../README.zh.md)

如何构建、测试和修改 PACE。

| 页面 | 内容 |
|:--|:--|
| [编写文档](documentation.zh.md) | 目录结构、翻译、reference 模板、`///` 注释 |
| [CI](ci.zh.md) | 每个工作流检查什么，在哪些平台上运行 |
| [Windows arm64 OpenSSL](windows-arm64-openssl.zh.md) | 为什么这个构建使用固定版本的 OpenSSL 包，以及问题是如何诊断出来的 |

## 构建

```sh
xmake f -m debug -y                                                  # 运行时、网络、HTTP/1.1、WebSocket
xmake f -m debug --tls=y --dns=y --quic=y --http2=y --http3=y -y     # 所有模块
xmake -y
```

每种配置使用单独的构建目录（`-o build/<name>`），这样切换配置时无需重新构建。在嵌套的 git worktree 中，每条 xmake
命令都要加上 `-P .`，否则 xmake 会找到上层检出目录里的工程。[全部选项](../reference/build-options.zh.md)

## 测试

```sh
xmake test                     # 当前配置下的所有测试程序
xmake run pace_network_tests   # 单个程序；-tc="name*" 等 doctest 选项可以筛选测试用例
```

测试使用 [doctest](https://github.com/doctest/doctest)，位于 `tests/<module>/`。每个测试用例都在看门狗下运行，
一旦卡住，看门狗就以退出码 124 结束进程。提交修改之前，请在它涉及的配置下运行测试；对运行时和套接字的修改，还要
包括 `--sanitizers=y`（AddressSanitizer 与 UndefinedBehaviorSanitizer）。测试不能依赖挂钟时间：要基于事件同步，
而不是靠 sleep。

CI 还会单独构建每个模块，并单独编译每个公开头文件（`--header_check=y`），因此头文件必须自行包含它用到的一切。

## 代码风格

- 格式化使用 clang-format 22.1.8 和仓库中的 `.clang-format`（Allman 大括号风格、120 列、每个控制语句都带大括号）：
  先 `pip install clang-format==22.1.8`，再用 `python3 scripts/format.py` 格式化，用
  `python3 scripts/format.py --check` 检查。
- CI 用 clang-tidy 22.1.8 和 `.clang-tidy` 检查每次修改涉及的行，出现任何警告即失败。某项检查不适用于某一行时，
  用 `// NOLINT(check): reason` 屏蔽，并且必须写明理由。
- 命名遵循标准库：类型、函数和变量用 `lower_case`，私有数据成员带后缀 `_`，模板参数用 `CamelCase`，宏以 `PACE_`
  开头。
- 公开声明带 `///` 注释；实现内部的注释说明“为什么”，而不是“做了什么”。
- 只有 `.cpp` 文件使用的头文件放在 `src/<module>/`，不放进 `include/`。公开头文件中的模板所需的辅助放进一个
  `*_access` 结构体，但协程除外：协程保持为命名空间作用域的函数，因为 GCC 16 无法从具名模块接口中读回类作用域的协程。
- 修改 `src/runtime/` 或 `src/net/` 中的热路径时，需要附上成对的基准测试对比（`scripts/compare_runtime_perf.py`）；
  看似无害的签名改动以前就改变过生成的代码。

## 模糊测试

```sh
xmake f -m releasedbg --toolchain=clang --fuzz=libfuzzer -y && xmake -y pace_fuzz_http1_server
xmake f -m debug --fuzz=replay -y && xmake -y && xmake test      # 不用 libFuzzer，回放种子语料
```

模糊测试目标在 `tests/fuzz/`，种子语料在 `tests/fuzz/corpus/`。每修复一个解析器缺陷，都要添加一个语料文件。

## 互操作性

```sh
python3 tests/interop/run_interop.py --build-dir build/<name> --list
python3 tests/interop/run_interop.py --build-dir build/<name>
```

运行脚本启动 `pace_interop_server`（随测试一起构建），并运行它找到的每个外部对端，例如 curl 和 h2spec；对端未安装
的测试套件会被跳过。对端和测试套件列在 `tests/interop/peers.json` 中。

## 基准测试

关于性能的结论，需要有按[性能](../performance/README.zh.md#数字是怎样得出的)中所述方法测得的数字。
`scripts/run_baseline.py` 运行所有测试套件；`scripts/compare_runtime_perf.py` 通过交替运行比较两个构建。页面引用
的结果连同原始数据放在 `benchmarks/results/<name>/` 下。

## 文档

每一页都有英文和中文两个版本；见[编写文档](documentation.zh.md)。提交文档修改之前，请运行
`python3 scripts/check_docs.py` 和 `mkdocs build --strict`。如果修改了 README 引用的基准测试结果，请用
`python3 scripts/render_readme_figures.py` 重新生成其中的图。

## 提交

每个提交只包含一个逻辑上的改动，主题行用祈使语气，正文说明改了什么、为什么改。大规模的机械性改动（格式化、移动
文件）单独提交，并加入 `.git-blame-ignore-revs`。
