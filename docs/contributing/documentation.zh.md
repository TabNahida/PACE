<!-- source-hash: ea66c4d0d386 -->
# 编写文档

[English](documentation.md) · [参与开发](README.zh.md)

文档是 `docs/` 下的 Markdown，在 GitHub 上可以直接阅读，同时用 [mkdocs](https://www.mkdocs.org/) 和
[Material](https://squidfunk.github.io/mkdocs-material/) 发布成站点。每一页都有英文和中文两个版本。

## 目录结构

页面按 [Diátaxis](https://diataxis.fr/) 划分，每一页只承担四种职责之一。

| 目录 | 职责 | 页面回答的问题 |
|:--|:--|:--|
| `tutorial/` | 学习 | “教我”：按编号顺序阅读的章节，每章结束时都有能运行的程序 |
| `guides/` | 解决问题 | “我该怎么……”：每页一个独立的配方 |
| `reference/` | 查阅 | “它究竟做什么”：每个模块一个目录，统一模板 |
| `explanation/` | 理解 | “为什么这样设计”：设计思路与取舍 |

`performance/` 存放测量报告（较早的在 `performance/history/`），`migration/` 是升级指南，`contributing/`
是项目的工作规范。目录的首页是 `README.md`：GitHub 打开目录时直接显示它，mkdocs 也把它作为该栏目的首页。

## 双语

- 英文页 `page.md` 旁边是中文页 `page.zh.md`，这是 [mkdocs-static-i18n](https://ultrabug.github.io/mkdocs-static-i18n/)
  的文件布局。
- 同一对页面的标题数量、层级和顺序完全一致，代码块也一一对应。中文页里的代码、标识符和签名保持英文，只翻译
  示例中的注释和正文。
- 英文页链接英文页，中文页链接中文页（`other.zh.md`）。每页开头放一个切换到另一种语言的链接。
- 中文页第一行记录它翻译的是哪个版本的英文页，即英文文件的 git blob 哈希：

  ```markdown
  <!-- source-hash: 3f1c2a9d0b7e -->
  ```

  译文与英文页一致后，运行 `python3 scripts/check_docs.py --stamp docs/page.zh.md` 写入哈希，再把两页一起提交。
  此后英文页一旦修改，`check_docs.py` 就会报告这对页面待同步，直到中文页更新并重新写入哈希。哈希只取决于文件内容，
  rebase 和 cherry-pick 都不会使它失效。
- 中文要按母语技术写作者的习惯来写，而不是逐句对译：使用全角标点；中英文之间是否加空格可以自选，但同一页内保持
  一致；采用通行术语（协程、反应器、取消、截止时间、背压），不自造译名。

## reference 页面

每个 reference 页面都按下列小节和顺序组织。某一节没有内容时，中英文两页都省略它。

| 英文 | 中文 | 内容 |
|:--|:--|:--|
| *（标题）* | *（标题）* | 模块或头文件名 |
| Overview | 概述 | 一句话说明用途，然后一张表：头文件、xmake target、构建选项 |
| Synopsis | 概要 | 一段能编译的简短典型用法 |
| Types | 类型 | 每个公开类型：声明及各成员的作用 |
| Functions | 函数 | 每个公开自由函数：签名、参数、结果 |
| Errors | 错误 | 会抛出或报告什么错误，在什么情况下 |
| Threads and concurrency | 线程与并发 | 哪个线程可以调用什么；同时可以有几个未完成的操作 |
| Lifetime and ownership | 生命周期与所有权 | 谁拥有什么；什么必须比什么活得久 |
| Limits and defaults | 限制与默认值 | 所有默认值和上限，用表格列出 |
| See also | 参见 | 相关的 reference 页面、指南和教程章节 |

概述中的表格形如：

```markdown
| Header | Target | Build option |
|:--|:--|:--|
| `<pace/tls.hpp>` | `pace_tls` | `--tls=y` |
```

- 只记录公开内容：`include/pace/` 下的头文件及其声明。内部头文件在 `src/` 中，不写进 reference。
- 签名从头文件复制，放在 `cpp` 代码块里，不带函数体。
- 页面里不写版本历史（“0.4.0 起”“新增于”）。版本之间的变化写进 `CHANGELOG.md` 和[迁移指南](../migration/README.zh.md)。
- 模块目录的 `README.md` 是模块概览：模块包含什么、有哪些页面，以及这些页面共同遵守的规则。

## 公开头文件中的注释

公开声明带 `///` 注释，写明 reference 所记录的约定：调用做什么、前置条件、会抛出什么、线程与生命周期要求。
注释要短，一行能说清的就只写一行。

```cpp
/// Completes when every byte of `bytes` has been written.
/// Throws std::system_error on a socket error. At most one write may be outstanding at a time.
task<void> write_all(std::span<const std::byte> bytes);
```

实现内部的注释解释“为什么”，而不是“做了什么”。

## 构建与检查

```sh
pip install -r docs/requirements.txt
mkdocs serve                       # http://127.0.0.1:8000，保存后自动重建
mkdocs build --strict              # CI 运行的命令
python3 scripts/check_docs.py      # 页面配对、结构、译文是否过期、链接与锚点
```

缺少译文、中英文结构不一致、链接或锚点失效时，`check_docs.py` 报错。译文过期在 1.0 之前只是警告，之后视为错误。
