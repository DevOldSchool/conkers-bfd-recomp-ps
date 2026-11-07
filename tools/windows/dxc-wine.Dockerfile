# The Windows DXC (dxc.exe) under Wine, for the Windows cross build's shaders (tools/windows/dxc_signed.sh):
# shaders compiled at build time must come from the same DXC as the dxcompiler.dll the game ships,
# or RT64 cannot link them at run time, and only Windows' dxc.exe matches that DLL's version.
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends wine64 \
    && rm -rf /var/lib/apt/lists/*
ENV WINEDEBUG=-all WINEPREFIX=/wine PATH=/usr/lib/wine:$PATH
RUN wine64 wineboot --init 2>/dev/null; wineserver -w; true
