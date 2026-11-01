#!/usr/bin/env bash
# CMAKE_CROSSCOMPILING_EMULATOR for the Windows cross build: a build step that runs one of RT64's
# tools (file_to_c.exe) runs the host build of it instead, from HOST_TOOLS_DIR.
set -euo pipefail
tool="$(basename "$1" .exe)"
shift
exec "$HOST_TOOLS_DIR/$tool" "$@"
