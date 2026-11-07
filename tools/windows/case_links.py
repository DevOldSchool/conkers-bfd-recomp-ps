#!/usr/bin/env python3
"""Links for the Windows SDK headers the source names in another case (tools/windows/cross_build.sh).

Windows does not care about a header name's case and the sources do not either (RT64 includes
"Shlobj.h", the SDK has ShlObj.h). xwin adds an all-lowercase link for every header, which covers
<windows.h>, but not a mixed-case name such as Shlobj.h or BaseTsd.h. On a case-sensitive disk (Linux)
this adds, next to each SDK header, a link for each other spelling of it that an #include in the
source uses. On a case-insensitive one (macOS) every spelling already exists and nothing is added.

    tools/windows/case_links.py <xwin folder> <source folder>...
"""

import os
import re
import sys
from pathlib import Path

INCLUDE = re.compile(rb'^\s*#\s*include\s*[<"]([A-Za-z0-9_.]+\.h)[>"]', re.MULTILINE)
SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl"}


def main() -> None:
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    xwin = Path(sys.argv[1])
    headers: dict[str, list[Path]] = {}
    for root in (xwin / "crt/include", xwin / "sdk/include"):
        for path in root.rglob("*.h"):
            headers.setdefault(path.name.lower(), []).append(path)

    names: set[str] = set()
    for source in sys.argv[2:]:
        for folder, dirs, files in os.walk(source):
            dirs[:] = [d for d in dirs if d != ".git"]
            for file in files:
                if Path(file).suffix.lower() in SUFFIXES:
                    try:
                        names.update(m.decode() for m in INCLUDE.findall(Path(folder, file).read_bytes()))
                    except OSError:
                        pass

    added = 0
    for name in sorted(names):
        for header in headers.get(name.lower(), []):
            link = header.with_name(name)
            if not os.path.lexists(link):
                link.symlink_to(header.name)
                added += 1
    print(f"case_links: {added} links for headers named in another case")


if __name__ == "__main__":
    main()
