# Builds build/<mod_filename>.nrm from src/*.c (see docs/modding.md).
#
#   make                 build the mod
#   make install DATA=<conker-recomp-data folder>   copy it into that folder's mods/
#
# Needs clang with the MIPS target and ld.lld (Homebrew: brew install llvm lld), and RecompModTool
# (cmake --build build/N64Recomp --target RecompModTool, from the repository root).

REPO ?= ../..
SDK := $(REPO)/mods
LLVM_BIN ?= $(firstword $(wildcard /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin))
CC := $(if $(LLVM_BIN),$(LLVM_BIN)/clang,clang)
LD := ld.lld
MOD_TOOL ?= $(REPO)/build/N64Recomp/RecompModTool

# MIPS II code in the o32 ABI, as the game's own. Singles stay out of the odd FPU registers
# (-mno-odd-spreg), so the code means the same in either FPU mode (the game runs with FR=1, where
# odd registers were tested to work as well). Without errno, sqrt and sqrtf are the FPU's
# instruction rather than calls to a C library mods do not have.
CFLAGS := -target mips -mips2 -mabi=32 -O2 -G0 -mno-abicalls -mno-odd-spreg -mno-check-zero-division \
          -fno-pic -fomit-frame-pointer -ffreestanding -fno-builtin -fno-math-errno -nostdinc \
          -Wall -Wextra -Wno-unused-parameter -I $(SDK)/include
LDFLAGS := -nostdlib -T $(SDK)/mod.ld -Map build/mod.map --unresolved-symbols=ignore-all --emit-relocs -e 0 --no-nmagic

SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:src/%.c=build/%.o)
MOD_NAME := $(shell sed -n 's/^mod_filename *= *"\(.*\)"/\1/p' mod.toml)

all: build/$(MOD_NAME).nrm

build/%.o: src/%.c $(wildcard $(SDK)/include/*.h) | build
	$(CC) $(CFLAGS) -c $< -o $@

build/mod.elf: $(OBJECTS) $(SDK)/mod.ld
	$(LD) $(LDFLAGS) $(OBJECTS) -o $@

build/$(MOD_NAME).nrm: build/mod.elf mod.toml $(REPO)/config/us.syms.toml $(REPO)/config/us.data.syms.toml
	$(MOD_TOOL) mod.toml build

build:
	mkdir -p build

install: build/$(MOD_NAME).nrm
	@test -n "$(DATA)" || (echo "usage: make install DATA=<conker-recomp-data folder>" && false)
	mkdir -p "$(DATA)/mods"
	cp build/$(MOD_NAME).nrm "$(DATA)/mods/"

clean:
	rm -rf build

.PHONY: all install clean
