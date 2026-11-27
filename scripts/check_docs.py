#!/usr/bin/env python3
"""Check the bilingual documentation: page pairs, matching structure, translation freshness and links.

Every English page `page.md` under docs/ (and the root README.md) has a Chinese page `page.zh.md`, and the
reverse. The two pages of a pair have the same heading levels in the same order and the same number of fenced
code blocks. A Chinese page starts with `<!-- source-hash: <hash> -->`, the git blob hash of the English page it
was translated from; any later change to the English page marks the pair as needing a sync. A content hash rather
than a commit, so that rebasing or cherry-picking the translation keeps it valid.

Every relative link in a checked Markdown file resolves to an existing file, and a link with an anchor names a
heading of its target (GitHub slug rules, which the site also uses).

Exit status is 1 on any error. Out-of-date translations are warnings unless --strict-sync is given.

`--stamp docs/page.zh.md ...` writes the current hash of each English page into its Chinese page; run it once the
translation matches the English page, and commit both together.
"""

import argparse
import re
import subprocess
import sys
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ZH = ".zh.md"
FENCE = re.compile(r"^(\s*)(`{3,}|~{3,})")
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
LINK = re.compile(r"(?<!!)\[(?:[^\]\[]|\[[^\]]*\])*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)|!\[[^\]]*\]\(([^)\s]+)\)"
                  r"|(?:href|src)=\"([^\"]+)\"")
SOURCE_HASH = re.compile(r"^<!--\s*source-hash:\s*([0-9a-f]{12,40})\s*-->")
HASH_LENGTH = 12
HTML_ID = re.compile(r"<a\s+(?:name|id)=\"([^\"]+)\"")


def git(*arguments):
    return subprocess.run(["git", *arguments], cwd=ROOT, capture_output=True, text=True, check=True).stdout


def tracked_markdown():
    files = []
    for name in git("ls-files", "-co", "--exclude-standard", "*.md").split():
        path = ROOT / name
        if not path.is_file():
            continue
        if name.startswith(("build/", "benchmarks/results/", "tests/results/")):
            continue
        files.append(path)
    return sorted(set(files))


def outside_fences(text):
    """Yield (line_number, line) for lines outside fenced code blocks, and count the fences."""
    fence = None
    for number, line in enumerate(text.splitlines(), 1):
        match = FENCE.match(line)
        if match:
            marker = match.group(2)
            if fence is None:
                fence = marker
            elif marker.startswith(fence[0]) and len(marker) >= len(fence) and not line.strip()[len(marker):]:
                fence = None
            continue
        if fence is None:
            yield number, line


def code_block_count(text):
    count = 0
    fence = None
    for line in text.splitlines():
        match = FENCE.match(line)
        if not match:
            continue
        marker = match.group(2)
        if fence is None:
            fence = marker
            count += 1
        elif marker.startswith(fence[0]) and len(marker) >= len(fence) and not line.strip()[len(marker):]:
            fence = None
    return count


def strip_inline(text):
    text = re.sub(r"!\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"<[^>]+>", "", text)
    return text


def slug(text):
    """GitHub's heading anchor: lower case, punctuation removed, spaces to hyphens."""
    text = strip_inline(text).strip().lower()
    kept = []
    for character in text:
        category = unicodedata.category(character)
        if character in " -_" or category[0] in "LN" or category == "Mn":
            kept.append(character)
    return "".join(kept).replace(" ", "-")


def headings(text):
    result = []
    for _, line in outside_fences(text):
        match = HEADING.match(line)
        if match:
            result.append((len(match.group(1)), match.group(2)))
    return result


def anchors(text):
    seen = {}
    result = set()
    for _, title in headings(text):
        base = slug(title)
        count = seen.get(base, 0)
        seen[base] = count + 1
        result.add(base if count == 0 else f"{base}-{count}")
    result.update(HTML_ID.findall(text))
    return result


class Checker:
    def __init__(self, strict_sync):
        self.strict_sync = strict_sync
        self.errors = []
        self.warnings = []
        self.anchor_cache = {}

    def error(self, path, message):
        self.errors.append(f"{path.relative_to(ROOT)}: {message}")

    def warn(self, path, message):
        self.warnings.append(f"{path.relative_to(ROOT)}: {message}")

    def anchors_of(self, path):
        if path not in self.anchor_cache:
            self.anchor_cache[path] = anchors(path.read_text(encoding="utf-8"))
        return self.anchor_cache[path]

    def check_links(self, path, text):
        for number, line in outside_fences(text):
            line = re.sub(r"`[^`]*`", "", line)
            for match in LINK.finditer(line):
                target = next(group for group in match.groups() if group)
                if re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", target) or target.startswith("//"):
                    continue
                location, _, anchor = target.partition("#")
                if location.startswith("/"):
                    self.error(path, f"line {number}: use a relative link instead of {target}")
                    continue
                resolved = (path.parent / location).resolve() if location else path
                if not resolved.exists():
                    self.error(path, f"line {number}: broken link {target}")
                    continue
                if resolved.is_dir():
                    readme = resolved / "README.md"
                    if anchor and readme.exists():
                        resolved = readme
                    elif anchor:
                        self.error(path, f"line {number}: anchor on a directory link {target}")
                        continue
                if anchor and resolved.suffix == ".md" and anchor not in self.anchors_of(resolved):
                    self.error(path, f"line {number}: no heading for anchor in {target}")

    def check_pair(self, english, chinese):
        english_text = english.read_text(encoding="utf-8")
        chinese_text = chinese.read_text(encoding="utf-8")
        english_levels = [level for level, _ in headings(english_text)]
        chinese_levels = [level for level, _ in headings(chinese_text)]
        if english_levels != chinese_levels:
            self.error(chinese, f"heading levels differ from {english.name}: {english_levels} vs {chinese_levels}")
        if code_block_count(english_text) != code_block_count(chinese_text):
            self.error(chinese, f"code block count differs from {english.name}: "
                                f"{code_block_count(english_text)} vs {code_block_count(chinese_text)}")
        first = chinese_text.splitlines()[0] if chinese_text else ""
        match = SOURCE_HASH.match(first)
        if not match:
            self.error(chinese, "first line must be <!-- source-hash: <hash> --> (run check_docs.py --stamp)")
            return
        current = content_hash(english)
        if not current.startswith(match.group(1)) and not match.group(1).startswith(current):
            message = f"{english.name} changed since it was translated (hash {match.group(1)}, now {current})"
            if self.strict_sync:
                self.error(chinese, message)
            else:
                self.warn(chinese, message)

    def run(self):
        files = tracked_markdown()
        for path in files:
            self.check_links(path, path.read_text(encoding="utf-8"))
        paired = [path for path in files if path == ROOT / "README.md" or path == ROOT / "README.zh.md"
                  or (ROOT / "docs") in path.parents]
        for path in paired:
            if path.name.endswith(ZH):
                english = path.with_name(path.name[: -len(ZH)] + ".md")
                if not english.exists():
                    self.error(path, f"no English page {english.name}")
            else:
                chinese = path.with_name(path.stem + ZH)
                if not chinese.exists():
                    self.error(path, f"no Chinese page {chinese.name}")
                else:
                    self.check_pair(path, chinese)
        for warning in self.warnings:
            print(f"warning: {warning}")
        for error in self.errors:
            print(f"error: {error}")
        print(f"{len(files)} Markdown files, {len(self.errors)} errors, {len(self.warnings)} warnings")
        return 1 if self.errors else 0


def content_hash(path):
    return git("hash-object", "--", str(path.relative_to(ROOT))).strip()[:HASH_LENGTH]


def stamp(pages):
    for name in pages:
        chinese = Path(name).resolve()
        if not chinese.name.endswith(ZH):
            sys.exit(f"{name}: not a Chinese page")
        english = chinese.with_name(chinese.name[: -len(ZH)] + ".md")
        if not english.exists():
            sys.exit(f"{name}: no English page {english.name}")
        line = f"<!-- source-hash: {content_hash(english)} -->"
        lines = chinese.read_text(encoding="utf-8").split("\n")
        if lines and SOURCE_HASH.match(lines[0]):
            lines[0] = line
        else:
            lines.insert(0, line)
        chinese.write_text("\n".join(lines), encoding="utf-8")
        print(f"{chinese.relative_to(ROOT)}: {line}")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--strict-sync", action="store_true", help="treat out-of-date translations as errors")
    parser.add_argument("--stamp", nargs="+", metavar="PAGE", help="record the English page hash in these Chinese pages")
    arguments = parser.parse_args()
    if arguments.stamp:
        return stamp(arguments.stamp)
    return Checker(arguments.strict_sync).run()


if __name__ == "__main__":
    sys.exit(main())
