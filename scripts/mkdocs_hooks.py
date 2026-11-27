"""mkdocs hooks that keep GitHub-friendly relative links working on the site.

- Links that leave docs/ (examples, sources, the root README) go to the repository on GitHub: the site has no
  copy of those files.
- An English page's link to a Chinese page (`page.zh.md`, the language switch at the top of a page) goes to
  that page on the Chinese site. mkdocs-static-i18n builds each language separately, so the English build has
  no `.zh.md` files to resolve against. Chinese pages linking to Chinese pages need nothing: the plugin
  resolves them in the Chinese build.
- A Chinese page's first line, the `<!-- source-hash: … -->` stamp that scripts/check_docs.py maintains, is dropped:
  mkdocs takes a page's title from its first element, which must be the `# heading`.
"""

import os
import re
from pathlib import PurePosixPath

REPOSITORY = "https://github.com/TabNahida/PACE"
LINK = re.compile(r"(\]\(|href=\"|src=\")([^)\"\s#]+)(#[^)\"\s]*)?")
FENCE = re.compile(r"^\s*(`{3,}|~{3,})")
STAMP = re.compile(r"\A<!--\s*source-hash:[^>]*-->\s*\n")


def on_page_markdown(markdown, page, config, files):
    markdown = STAMP.sub("", markdown)
    docs = PurePosixPath(config["docs_dir"]).name
    source = PurePosixPath(docs) / PurePosixPath(page.file.src_uri).parent
    english = not page.file.src_uri.endswith(".zh.md")
    site = config.get("site_url") or "/"

    def rewrite(match):
        target = match.group(2)
        if re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", target) or target.startswith("/"):
            return match.group(0)
        resolved = PurePosixPath(os.path.normpath((source / target).as_posix()))
        if resolved.parts and resolved.parts[0] == docs:
            if english and resolved.name.endswith(".zh.md"):
                inside = PurePosixPath(*resolved.parts[1:])
                stem = inside.name[: -len(".zh.md")]
                url = inside.parent if stem == "README" else inside.parent / stem
                path = "" if str(url) == "." else f"{url.as_posix()}/"
                return f"{match.group(1)}{site.rstrip('/')}/zh/{path}{match.group(3) or ''}"
            return match.group(0)
        kind = "tree" if not resolved.suffix else "blob"
        return f"{match.group(1)}{REPOSITORY}/{kind}/main/{resolved.as_posix()}{match.group(3) or ''}"

    lines = []
    fence = None
    for line in markdown.split("\n"):
        marker = FENCE.match(line)
        if marker:
            fence = None if fence and marker.group(1).startswith(fence) else (fence or marker.group(1))
            lines.append(line)
            continue
        lines.append(line if fence else LINK.sub(rewrite, line))
    return "\n".join(lines)
