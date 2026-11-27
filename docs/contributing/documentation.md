# Writing documentation

[简体中文](documentation.zh.md) · [Contributing](README.md)

The documentation is Markdown under `docs/`. It reads on GitHub as it is and is published as a site with
[mkdocs](https://www.mkdocs.org/) and [Material](https://squidfunk.github.io/mkdocs-material/). Every page exists
in English and Chinese.

## Layout

The pages follow [Diátaxis](https://diataxis.fr/): each page does one of four jobs.

| Directory | Job | A page answers |
|:--|:--|:--|
| `tutorial/` | Learning | "Teach me": numbered chapters, read in order, each ending with something that runs |
| `guides/` | Solving a problem | "How do I …": one self-contained recipe per page |
| `reference/` | Looking up | "What exactly does this do": one directory per module, one fixed template |
| `explanation/` | Understanding | "Why is it like this": design and trade-offs |

`performance/` holds measurement reports (older ones under `performance/history/`), `migration/` the upgrade
guides and `contributing/` the project's working rules. A directory's index page is `README.md`: GitHub shows it
when the directory is opened and mkdocs uses it as the section index.

## Two languages

- An English page `page.md` has a Chinese page `page.zh.md` next to it, the file layout of
  [mkdocs-static-i18n](https://ultrabug.github.io/mkdocs-static-i18n/).
- The two pages of a pair have the same headings at the same levels in the same order and the same fenced code
  blocks. Code, identifiers and signatures stay in English on the Chinese page; only comments in examples and
  the prose are translated.
- English pages link to English pages, Chinese pages to Chinese pages (`other.zh.md`). Each page starts with a
  link to the other language.
- A Chinese page's first line records which version of the English page it translates, as the English file's git
  blob hash:

  ```markdown
  <!-- source-hash: 3f1c2a9d0b7e -->
  ```

  Once the translation matches the English page, run `python3 scripts/check_docs.py --stamp docs/page.zh.md` to
  write the hash, and commit both pages together. A later change to the English page makes `check_docs.py` report
  the pair as out of date until the Chinese page is updated and stamped again. The hash depends only on the
  file's content, so rebases and cherry-picks keep it valid.
- Write Chinese as a native technical writer would, not as a sentence-by-sentence rendering: full-width
  punctuation, a space between Chinese and Latin text is optional but used consistently within a page, and
  established terms (协程, 反应器, 取消, 截止时间, 背压) rather than invented ones.

## Reference pages

Every reference page uses these sections, in this order. Leave out a section that has nothing to say, in both
languages.

| English | Chinese | Contents |
|:--|:--|:--|
| *(title)* | *(title)* | The module or header name |
| Overview | 概述 | One sentence on what it is for, then a table: header, xmake target, build option |
| Synopsis | 概要 | One short compiling example of typical use |
| Types | 类型 | Each public type: its declaration and what each member does |
| Functions | 函数 | Each public free function: signature, parameters, result |
| Errors | 错误 | What can be thrown or reported, and when |
| Threads and concurrency | 线程与并发 | Which thread may call what; how many operations may be outstanding |
| Lifetime and ownership | 生命周期与所有权 | Who owns what; what must outlive what |
| Limits and defaults | 限制与默认值 | Every default value and bound, in a table |
| See also | 参见 | Related reference pages, guides and tutorial chapters |

The overview table looks like this:

```markdown
| Header | Target | Build option |
|:--|:--|:--|
| `<pace/tls.hpp>` | `pace_tls` | `--tls=y` |
```

- Document what is public: the headers under `include/pace/` and what they declare. Internal headers live in
  `src/` and are not documented in the reference.
- Signatures are copied from the headers, in a `cpp` code block, without bodies.
- No version history in pages ("since 0.4.0", "new in"). Changes between versions belong in `CHANGELOG.md` and
  the [migration guides](../migration/README.md).
- A module directory's `README.md` is the module overview: what the module contains, its pages, and the rules
  shared by all of them.

## Comments in public headers

Public declarations carry `///` comments. They state the contract the reference documents: what the call does,
its preconditions, what it throws, and its thread and lifetime requirements. They are short: one line where one
line is enough.

```cpp
/// Completes when every byte of `bytes` has been written.
/// Throws std::system_error on a socket error. At most one write may be outstanding at a time.
task<void> write_all(std::span<const std::byte> bytes);
```

Comments inside implementations explain why, not what.

## Build and check

```sh
pip install -r docs/requirements.txt
mkdocs serve                       # http://127.0.0.1:8000, rebuilt on save
mkdocs build --strict              # what CI runs
python3 scripts/check_docs.py      # pairs, structure, translation freshness, links and anchors
```

`check_docs.py` fails on a missing translation, on structure that differs between the two languages, and on a
broken link or anchor. Out-of-date translations are warnings until 1.0, errors afterwards.
