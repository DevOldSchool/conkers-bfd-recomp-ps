# The PS5 build environment for tools/ps5/build.sh: what PS5_Vulkan's dependency setup and RADV build
# need (tools/setup-native-dependencies.sh, tools/build-radv.sh), and the game's own CMake build. Built
# for the host's architecture: arm64 on Apple silicon, x86-64 on a Linux CI machine. The console is
# x86-64 either way; clang cross-compiles for it.
FROM ubuntu:24.04

# clang 18 and its tools: the payload SDK's wrappers find them through llvm-config-18
# (prospero-llvm-config). LLVM, Clang, libclc, the SPIR-V translator and SPIRV-Tools are for the two
# host tools RADV's build compiles its OpenCL kernels with (mesa_clc, vtn_bindgen2). glslang builds
# RADV's own GLSL shaders.
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        clang-18 lld-18 llvm-18 llvm-18-dev llvm-18-tools libclang-18-dev libclang-cpp18-dev \
        libclang-rt-18-dev libclc-18 libclc-18-dev llvm-spirv-18 libllvmspirvlib-18-dev \
        spirv-tools glslang-tools \
        cmake ninja-build make pkg-config bison flex python3 python3-venv python3-dev \
        git curl wget unzip ca-certificates rsync ccache file xz-utils zlib1g-dev util-linux \
    && rm -rf /var/lib/apt/lists/* \
    && git config --system --add safe.directory '*' \
    && for tool in clang clang++ ld.lld llvm-ar llvm-nm llvm-ranlib llvm-objcopy llvm-strip llvm-config; do \
           ln -sf "/usr/bin/$tool-18" "/usr/local/bin/$tool"; \
       done

# The console is x86-64: RADV's link takes clang's x86-64 builtins (__emutls_get_address and the
# like, tools/radv-link.sh), which an arm64 clang does not ship. They come from Ubuntu's amd64 build
# of the same package, unpacked beside the arm64 ones rather than installed (the two packages would
# clash).
RUN if [ "$(dpkg --print-architecture)" = arm64 ]; then \
        set -e; \
        sed -i 's/^Types: deb$/Types: deb\nArchitectures: arm64/' /etc/apt/sources.list.d/ubuntu.sources; \
        printf '%s\n' 'Types: deb' 'URIs: http://archive.ubuntu.com/ubuntu/' \
            'Suites: noble noble-updates noble-security' 'Components: main universe' \
            'Architectures: amd64' 'Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg' \
            > /etc/apt/sources.list.d/amd64.sources; \
        dpkg --add-architecture amd64; \
        apt-get update; \
        cd /tmp; \
        apt-get download libclang-rt-18-dev:amd64; \
        dpkg-deb -x libclang-rt-18-dev_*_amd64.deb /tmp/rt; \
        resource="$(clang-18 --print-resource-dir)"; \
        mkdir -p "$resource/lib/linux"; \
        cp /tmp/rt/usr/lib/llvm-18/lib/clang/18/lib/linux/libclang_rt.builtins-x86_64.a "$resource/lib/linux/"; \
        rm -rf /tmp/rt /tmp/*.deb /var/lib/apt/lists/*; \
    fi \
    && test -f "$(clang-18 --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a"

# Python for Mesa's build (meson, its mako templates) and for the home-screen pictures
# (tools/ps5/make_home_art.py: Pillow, and etcpak for BC7), pinned.
RUN python3 -m venv /opt/venv \
    && /opt/venv/bin/pip install --no-cache-dir \
        meson==1.6.1 mako==1.3.8 markupsafe==3.0.2 pyyaml==6.0.2 packaging==24.2 \
        pillow==11.1.0 etcpak==0.9.13
ENV PATH=/opt/venv/bin:$PATH

ENV CC=clang CXX=clang++
