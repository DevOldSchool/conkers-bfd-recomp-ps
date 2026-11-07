#!/usr/bin/env python3
"""Writes the license files a packaged build must carry into OUT_DIR.

    tools/package_notices.py BUILD_DIR OUT_DIR

The packaging scripts (tools/package_macos.sh, tools/linux/build.sh, tools/windows/package.sh) run it. It writes:

* LICENSE.txt: this project's own code (MIT).
* COPYING.txt: the GPL-3.0. N64ModernRuntime is GPL-3.0 and is linked into the binary, so the
  binary as a whole is passed on under the GPL-3.0.
* THIRD-PARTY-NOTICES.txt: the copyright and license of every library compiled into the binary or
  shipped beside it, read from the checked-out submodules and from BUILD_DIR/_deps (the libraries
  CMake fetches), so it follows them when they are updated. RT64's DXC binaries come without their
  licenses, so tools/licenses/dxc holds them, from the DXC v1.8.2403.2 release.
* SOURCE.txt: the exact source the build came from, as GPL-3.0 section 6 requires.

A library added to the build needs an entry in COMPONENTS. It fails rather than leave a license out.
"""

from __future__ import annotations

import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
REPO_URL = "https://github.com/DevOldSchool/conkers-bfd-recomp"

MIT = """Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE."""

ZLIB = """This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution."""


@dataclass
class Component:
    name: str
    url: str
    license: str
    files: tuple[str, ...] = ()  # license files, relative to the repo ("@deps/" = BUILD_DIR/_deps/)
    header: tuple[str, str, str] | None = None  # (source file, first line, last line) of a license comment
    text: str | None = None  # the license text, for code that carries only a copyright line
    platforms: tuple[str, ...] = ("macos", "linux", "windows")


SDL_COPYRIGHT = "Copyright (C) 1997-2025 Sam Lantinga"

COMPONENTS = [
    Component("N64ModernRuntime (librecomp, ultramodern)", "https://github.com/N64Recomp/N64ModernRuntime",
              "GPL-3.0", text="The full text is in COPYING.txt."),
    Component("N64Recomp (the live recompiler used for mods)", "https://github.com/N64Recomp/N64Recomp",
              "MIT", ("lib/N64ModernRuntime/N64Recomp/LICENSE",)),
    Component("rabbitizer", "https://github.com/Decompollaborate/rabbitizer", "MIT",
              ("lib/N64ModernRuntime/N64Recomp/lib/rabbitizer/LICENSE",)),
    Component("{fmt}", "https://github.com/fmtlib/fmt", "MIT",
              ("lib/N64ModernRuntime/N64Recomp/lib/fmt/LICENSE",)),
    Component("SLJIT", "https://github.com/zherczeg/sljit", "BSD-2-Clause",
              ("lib/N64ModernRuntime/N64Recomp/lib/sljit/LICENSE",)),
    Component("miniz (N64ModernRuntime's copy)", "https://github.com/richgel999/miniz", "MIT",
              ("lib/N64ModernRuntime/thirdparty/miniz/LICENSE",)),
    Component("xxHash (N64ModernRuntime's copy)", "https://github.com/Cyan4973/xxHash", "BSD-2-Clause",
              ("lib/N64ModernRuntime/thirdparty/xxHash/LICENSE",)),
    Component("o1heap", "https://github.com/pavel-kirienko/o1heap", "MIT",
              ("lib/N64ModernRuntime/thirdparty/o1heap/LICENSE",)),
    Component("moodycamel::ConcurrentQueue", "https://github.com/cameron314/concurrentqueue", "BSD-2-Clause",
              header=("lib/N64ModernRuntime/thirdparty/concurrentqueue/concurrentqueue.h",
                      "Simplified BSD license:", "EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.")),
    Component("sse2neon", "https://github.com/DLTcollab/sse2neon", "MIT",
              header=("lib/N64ModernRuntime/thirdparty/sse2neon/sse2neon.h",
                      "sse2neon is freely redistributable", "SOFTWARE.")),
    Component("JSON for Modern C++", "https://github.com/nlohmann/json", "MIT",
              text="Copyright (c) 2013-2025 Niels Lohmann\n\n" + MIT),
    Component("RT64", "https://github.com/rt64/rt64", "MIT", ("lib/rt64/LICENSE",)),
    Component("plume", "https://github.com/renderbag/plume", "MIT", ("lib/rt64/src/contrib/plume/LICENSE",)),
    Component("metal-cpp", "https://developer.apple.com/metal/cpp/", "Apache-2.0",
              ("lib/rt64/src/contrib/plume/contrib/metal-cpp/LICENSE.txt",), platforms=("macos",)),
    Component("Vulkan-Headers", "https://github.com/KhronosGroup/Vulkan-Headers", "Apache-2.0 or MIT",
              ("lib/rt64/src/contrib/plume/contrib/Vulkan-Headers/LICENSE.md",)),
    Component("volk", "https://github.com/zeux/volk", "MIT",
              ("lib/rt64/src/contrib/plume/contrib/volk/LICENSE.md",), platforms=("linux", "windows")),
    Component("Vulkan Memory Allocator", "https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator",
              "MIT", ("lib/rt64/src/contrib/plume/contrib/VulkanMemoryAllocator/LICENSE.txt",),
              platforms=("linux", "windows")),
    Component("D3D12 Memory Allocator", "https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator",
              "MIT", ("lib/rt64/src/contrib/plume/contrib/D3D12MemoryAllocator/LICENSE.txt",),
              platforms=("windows",)),
    Component("HLSL++", "https://github.com/redorav/hlslpp", "MIT", ("lib/rt64/src/contrib/hlslpp/LICENSE",)),
    Component("Dear ImGui", "https://github.com/ocornut/imgui", "MIT", ("lib/rt64/src/contrib/imgui/LICENSE.txt",)),
    Component("ImPlot", "https://github.com/epezent/implot", "MIT", ("lib/rt64/src/contrib/implot/LICENSE",)),
    Component("im3d", "https://github.com/john-chapman/im3d", "MIT", ("lib/rt64/src/contrib/im3d/LICENSE",)),
    Component("SPIRV-Cross", "https://github.com/KhronosGroup/SPIRV-Cross", "Apache-2.0",
              ("lib/rt64/src/contrib/spirv-cross/LICENSE",)),
    Component("re-spirv", "https://github.com/rt64/re-spirv", "MIT", ("lib/rt64/src/contrib/re-spirv/LICENSE",)),
    Component("SPIRV-Headers", "https://github.com/KhronosGroup/SPIRV-Headers", "MIT",
              ("lib/rt64/src/contrib/re-spirv/external/SPIRV-Headers/LICENSE",)),
    Component("Zstandard", "https://github.com/facebook/zstd", "BSD-3-Clause",
              ("lib/rt64/src/contrib/zstd/LICENSE",)),
    Component("xxHash (RT64's copy)", "https://github.com/Cyan4973/xxHash", "BSD-2-Clause",
              ("lib/rt64/src/contrib/xxHash/LICENSE",)),
    Component("miniz (RT64's copy)", "https://github.com/richgel999/miniz", "MIT",
              header=("lib/rt64/src/contrib/miniz/miniz.c", "Copyright 2013-2014 RAD Game Tools", "THE SOFTWARE.")),
    Component("stb", "https://github.com/nothings/stb", "MIT or public domain", ("lib/rt64/src/contrib/stb/LICENSE",)),
    Component("ddspp", "https://github.com/redorav/ddspp", "MIT", ("lib/rt64/src/contrib/ddspp/LICENSE",)),
    Component("Native File Dialog Extended", "https://github.com/btzy/nativefiledialog-extended", "Zlib",
              ("lib/rt64/src/contrib/nativefiledialog-extended/LICENSE",)),
    Component("miniupnpc", "https://github.com/miniupnp/miniupnp", "BSD-3-Clause",
              ("@deps/miniupnpc-src/LICENSE",)),
    Component("libnatpmp", "https://github.com/miniupnp/libnatpmp", "BSD-3-Clause",
              ("@deps/libnatpmp-src/LICENSE",)),
    Component("Monocypher", "https://monocypher.org", "BSD-2-Clause or CC0-1.0",
              ("@deps/monocypher-src/LICENCE.md",)),
    Component("Super-xBR (the sharper text and menus are a port of it)", "https://github.com/libretro/glsl-shaders",
              "MIT", text="Copyright (c) 2015 Hyllian\n\n" + MIT),
    Component("SDL 2 (sdl2-compat on macOS, where it is in the app)", "https://www.libsdl.org", "Zlib",
              text=SDL_COPYRIGHT + "\n\n" + ZLIB, platforms=("macos", "linux")),
    Component("SDL 2 (SDL2.dll)", "https://www.libsdl.org", "Zlib",
              text=SDL_COPYRIGHT + "\n\n" + ZLIB, platforms=("windows",)),
    Component("DirectX Shader Compiler (dxcompiler.dll)", "https://github.com/microsoft/DirectXShaderCompiler",
              "NCSA and MIT", ("tools/licenses/dxc/LICENSE-LLVM.txt", "tools/licenses/dxc/LICENSE-MIT.txt"),
              platforms=("windows",)),
    Component("DirectX Shader Compiler's validator (dxil.dll)", "https://github.com/microsoft/DirectXShaderCompiler",
              "Microsoft Software License Terms (redistributable, closed source)",
              ("tools/licenses/dxc/LICENSE-MS.txt",), platforms=("windows",)),
    Component("SDL 3 (in the app, loaded by sdl2-compat)", "https://www.libsdl.org", "Zlib",
              text=SDL_COPYRIGHT + "\n\n" + ZLIB, platforms=("macos",)),
]


def header_text(path: Path, first: str, last: str) -> str:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    start = next(i for i, line in enumerate(lines) if first in line)
    end = next(i for i in range(start, len(lines)) if last in lines[i])
    out = []
    for line in lines[start:end + 1]:
        stripped = line.strip()
        for marker in ("//", "*"):
            if stripped.startswith(marker):
                stripped = stripped[len(marker):]
                break
        out.append(stripped[1:] if stripped.startswith(" ") else stripped)
    return "\n".join(out)


def license_text(component: Component, deps: Path) -> str:
    if component.text is not None:
        return component.text
    if component.header is not None:
        source, first, last = component.header
        return header_text(REPO / source, first, last)
    parts = []
    for name in component.files:
        path = deps / name[len("@deps/"):] if name.startswith("@deps/") else REPO / name
        if not path.is_file():
            sys.exit(f"package_notices.py: no license file {path} for {component.name}")
        parts.append(path.read_text(encoding="utf-8", errors="replace").strip())
    return "\n\n".join(parts)


def git(*args: str, cwd: Path = REPO) -> str:
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def source_text(version: str) -> str:
    commit = git("rev-parse", "HEAD")
    dirty = bool(git("status", "--porcelain", "--untracked-files=no", "--ignore-submodules=all"))
    submodules = []
    # The revision each submodule's parent records, not whatever is checked out there.
    for line in git("submodule", "foreach", "--quiet", "--recursive", 'echo "$displaypath $sha1"').splitlines():
        path, sha = line.split()
        submodules.append(f"  {path} at {sha}")
    note = ""
    if dirty:
        print("package_notices.py: warning: built from uncommitted changes; SOURCE.txt says so", file=sys.stderr)
        note = ("\nThis build was made from uncommitted changes on top of that commit: it is a test build,\n"
                "not one to pass on.\n")
    return f"""Conker's Bad Fur Day Recompiled {version}

The program's source code, under the licenses in LICENSE.txt, COPYING.txt and
THIRD-PARTY-NOTICES.txt, is at

  {REPO_URL}

This build was made from commit

  {commit}
  {REPO_URL}/tree/{commit}
{note}
with these submodules (git submodule update --init --recursive checks them out), changed by
the patches in tools/patches (tools/apply_patches.sh applies them):

{chr(10).join(submodules)}

The build fetches miniupnpc, libnatpmp and Monocypher at the versions pinned in CMakeLists.txt.
docs/building-macos.md, docs/building-linux.md and docs/building-windows.md say how to build it.
Every release also has the complete source, submodules included, as a download beside the build.

The game code is not in that source. The build makes it from your own ROM with N64Recomp
(tools/regenerate.sh). Conker's Bad Fur Day and its code, art and sound belong to Rare and
Microsoft; this project is not affiliated with or endorsed by them.
"""


def main() -> None:
    if len(sys.argv) != 4 or sys.argv[3] not in ("macos", "linux", "windows"):
        sys.exit("usage: tools/package_notices.py BUILD_DIR OUT_DIR macos|linux|windows")
    deps = Path(sys.argv[1]) / "_deps"
    out = Path(sys.argv[2])
    platform = sys.argv[3]
    out.mkdir(parents=True, exist_ok=True)
    version = subprocess.run([str(REPO / "tools/version.sh")], check=True, capture_output=True, text=True).stdout.strip()

    (out / "LICENSE.txt").write_text((REPO / "LICENSE").read_text(encoding="utf-8"), encoding="utf-8")
    (out / "COPYING.txt").write_text((REPO / "lib/N64ModernRuntime/COPYING").read_text(encoding="utf-8"),
                                     encoding="utf-8")
    sections = [
        "Conker's Bad Fur Day Recompiled includes the following software. This project's own code is\n"
        "under the MIT license (LICENSE.txt). N64ModernRuntime is under the GPL-3.0 (COPYING.txt), and\n"
        "since it is linked into the program, the program as a whole is passed on under the GPL-3.0.\n"
        "Its source code: SOURCE.txt."
    ]
    for component in COMPONENTS:
        if platform not in component.platforms:
            continue
        rule = "=" * 78
        sections.append(f"{rule}\n{component.name}\n{component.url}\nLicense: {component.license}\n{rule}\n\n"
                        + license_text(component, deps))
    (out / "THIRD-PARTY-NOTICES.txt").write_text("\n\n\n".join(sections) + "\n", encoding="utf-8")
    (out / "SOURCE.txt").write_text(source_text(version), encoding="utf-8")
    print(f"==> licenses and source notice in {out}")


if __name__ == "__main__":
    main()
