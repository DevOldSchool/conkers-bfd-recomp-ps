#!/usr/bin/env python3
"""Links for the Windows SDK headers and libraries the source names in another case
(tools/windows/cross_build.sh).

Windows does not care about a file name's case and the sources do not either (RT64 includes
"Shlobj.h" and links Shcore.lib; the SDK has ShlObj.h and shcore.lib). xwin adds an all-lowercase
link for every header and library, which covers <windows.h>, but not a mixed-case name. On a
case-sensitive disk (Linux) this adds, next to each SDK header or library, a link for each other
spelling of it that the source uses: in an #include, in a CMake file (Shcore.lib, Ws2_32) or in a
#pragma comment(lib, ...). On a case-insensitive disk (macOS) every spelling already exists and
nothing is added.

    tools/windows/case_links.py <xwin folder> <source folder or file>...
"""

import os
import re
import sys
from pathlib import Path

INCLUDE = re.compile(rb'^\s*#\s*include\s*[<"]([A-Za-z0-9_.]+\.h)[>"]', re.MULTILINE)
PRAGMA_LIB = re.compile(rb'#\s*pragma\s+comment\s*\(\s*lib\s*,\s*"([A-Za-z0-9_.]+)"', re.IGNORECASE)
WORD = re.compile(rb"[A-Za-z0-9_]+(?:\.lib)?")
SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl"}


def lib_name(word: str) -> str:
    return word if word.lower().endswith(".lib") else word + ".lib"


def main() -> None:
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    xwin = Path(sys.argv[1])
    headers: dict[str, list[Path]] = {}
    for root in (xwin / "crt/include", xwin / "sdk/include"):
        for path in root.rglob("*.h"):
            headers.setdefault(path.name.lower(), []).append(path)

    libraries: dict[str, list[Path]] = {}
    for root in (xwin / "crt/lib", xwin / "sdk/lib"):
        for path in root.rglob("*.lib"):
            libraries.setdefault(path.name.lower(), []).append(path)

    names: set[str] = set()
    library_names: set[str] = set()
    for source in sys.argv[2:]:
        walk = [(str(Path(source).parent), [], [Path(source).name])] if Path(source).is_file() else os.walk(source)
        for folder, dirs, files in walk:
            dirs[:] = [d for d in dirs if d != ".git"]
            for file in files:
                suffix = Path(file).suffix.lower()
                cmake = file == "CMakeLists.txt" or suffix == ".cmake"
                if suffix not in SUFFIXES and not cmake:
                    continue
                try:
                    text = Path(folder, file).read_bytes()
                except OSError:
                    continue
                if cmake:
                    library_names.update(lib_name(m.decode()) for m in WORD.findall(text))
                else:
                    names.update(m.decode() for m in INCLUDE.findall(text))
                    library_names.update(lib_name(m.decode()) for m in PRAGMA_LIB.findall(text))

    added = 0
    for wanted, found in ((names, headers), (library_names, libraries)):
        for name in sorted(wanted):
            for path in found.get(name.lower(), []):
                link = path.with_name(name)
                if not os.path.lexists(link):
                    link.symlink_to(path.name)
                    added += 1
    print(f"case_links: {added} links for headers and libraries named in another case")


if __name__ == "__main__":
    main()
