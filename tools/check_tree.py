#!/usr/bin/env python3
"""Check the tracked files (and optionally commit messages) for things a public repository must not carry.

Fails on:
  * a ROM, by extension or by the N64 header in any byte order, or a file over the size limit
  * a home folder path (/Users/<name>, /home/<name>, C:\\Users\\<name>)
  * an email address, other than the few placeholders and no-reply addresses allowed below
  * a match for any pattern in the file named by CONKER_PRIVATE_PATTERNS (one regular expression
    a line, for names that must not appear; kept out of the repository, as a CI secret)

  tools/check_tree.py                      the tracked files
  tools/check_tree.py --commits A..B       also the messages of those commits
"""

import argparse
import os
import re
import subprocess
import sys

MAX_BYTES = 2 * 1024 * 1024
ROM_EXTENSIONS = {".z64", ".n64", ".v64", ".rom", ".bin"}
# The first word of an N64 ROM in big-endian (z64), byte-swapped (v64) and little-endian (n64) order.
ROM_MAGICS = (b"\x80\x37\x12\x40", b"\x37\x80\x40\x12", b"\x40\x12\x37\x80")

HOME_PATH = re.compile(rb"/Users/(?!Shared\b)[A-Za-z0-9._-]+|/home/[a-z_][a-z0-9_-]*|\b[A-Za-z]:\\\\?Users\\\\?[A-Za-z0-9._-]+")
EMAIL = re.compile(rb"[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}")
ALLOWED_EMAIL = re.compile(rb"^(?:git@github\.com|noreply@anthropic\.com|[^@]+@users\.noreply\.github\.com|[^@]+@example\.(?:com|org))$")


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True).stdout


def private_patterns():
    path = os.environ.get("CONKER_PRIVATE_PATTERNS")
    if not path:
        return []
    with open(path, "rb") as f:
        return [re.compile(line.strip(), re.IGNORECASE) for line in f if line.strip()]


def text_problems(data, patterns):
    """(line number, what) for each problem in a text."""
    found = []
    for number, line in enumerate(data.splitlines(), 1):
        for match in HOME_PATH.finditer(line):
            found.append((number, "home path " + match.group().decode(errors="replace")))
        for match in EMAIL.finditer(line):
            if not ALLOWED_EMAIL.match(match.group()):
                found.append((number, "email address " + match.group().decode(errors="replace")))
        for pattern in patterns:
            if pattern.search(line):
                found.append((number, "private pattern"))  # the pattern itself is not printed
    return found


def check_files(patterns):
    problems = []
    for entry in git("ls-files", "-s", "-z").split(b"\0"):
        if not entry:
            continue
        mode, _, rest = entry.partition(b" ")
        if mode == b"160000":  # a submodule
            continue
        path = rest.split(b"\t", 1)[1].decode()
        if not os.path.isfile(path):  # deleted in the work tree
            continue
        size = os.path.getsize(path)
        if os.path.splitext(path)[1].lower() in ROM_EXTENSIONS:
            problems.append(f"{path}: ROM-like file extension")
        if size > MAX_BYTES:
            problems.append(f"{path}: {size} bytes, over the {MAX_BYTES} byte limit")
        with open(path, "rb") as f:
            data = f.read()
        if data[:4] in ROM_MAGICS:
            problems.append(f"{path}: starts with an N64 ROM header")
        if b"\0" in data[:8192]:  # binary: only the ROM checks apply
            continue
        problems += [f"{path}:{line}: {what}" for line, what in text_problems(data, patterns)]
    return problems


def check_commits(commits, patterns):
    problems = []
    for sha in git("rev-list", commits).split():
        message = git("log", "-1", "--format=%B", sha.decode())
        problems += [f"commit {sha.decode()[:10]} message line {line}: {what}" for line, what in text_problems(message, patterns)]
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--commits", help="a revision range whose commit messages are checked too, such as main..HEAD")
    args = parser.parse_args()

    os.chdir(git("rev-parse", "--show-toplevel").decode().strip())
    patterns = private_patterns()
    problems = check_files(patterns)
    if args.commits:
        problems += check_commits(args.commits, patterns)

    for problem in problems:
        print(problem)
    if problems:
        print(f"{len(problems)} problem(s): remove them before pushing", file=sys.stderr)
        return 1
    print("tree check: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
