#!/usr/bin/env python3
"""Check the relative links in README.md and docs/*.md: the file must exist, and a #heading must be one
of its headings (GitHub's anchors). Links to other sites are not fetched."""

import os
import re
import subprocess
import sys

LINK = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
HEADING = re.compile(r"^#{1,6}\s+(.*?)\s*#*\s*$")
FENCE = re.compile(r"^\s*(```|~~~)")


def strip_code(text, inline=True):
    """The text with fenced blocks (and inline code) blanked, so links shown as code are not checked."""
    lines, fenced = [], False
    for line in text.splitlines():
        if FENCE.match(line):
            fenced = not fenced
            lines.append("")
            continue
        lines.append("" if fenced else re.sub(r"`[^`]*`", "", line) if inline else line)
    return lines


def anchors(path):
    """GitHub's anchor for each heading of a markdown file."""
    seen, result = {}, set()
    with open(path, encoding="utf-8") as f:
        for line in strip_code(f.read(), inline=False):
            match = HEADING.match(line)
            if not match:
                continue
            slug = re.sub(r"[^\w\- ]", "", match.group(1).lower()).replace(" ", "-")
            count = seen.get(slug, 0)
            seen[slug] = count + 1
            result.add(slug if count == 0 else f"{slug}-{count}")
    return result


def main():
    os.chdir(subprocess.run(["git", "rev-parse", "--show-toplevel"], check=True, capture_output=True, text=True).stdout.strip())
    files = ["README.md"] + sorted(os.path.join("docs", name) for name in os.listdir("docs") if name.endswith(".md"))
    problems = []
    for source in files:
        with open(source, encoding="utf-8") as f:
            lines = strip_code(f.read())
        for number, line in enumerate(lines, 1):
            for target in LINK.findall(line):
                if re.match(r"^[a-z][a-z0-9+.-]*:", target, re.IGNORECASE):  # http:, mailto: and so on
                    continue
                path, _, anchor = target.partition("#")
                path = re.sub(r":\d+(?:-\d+)?$", "", path)  # file.cpp:42 line suffixes
                resolved = os.path.normpath(os.path.join(os.path.dirname(source), path)) if path else source
                if not os.path.exists(resolved):
                    problems.append(f"{source}:{number}: {target}: no such file")
                elif anchor and resolved.endswith(".md") and anchor.lower() not in anchors(resolved):
                    problems.append(f"{source}:{number}: {target}: no such heading")

    for problem in problems:
        print(problem)
    if problems:
        return 1
    print(f"links: ok ({len(files)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
