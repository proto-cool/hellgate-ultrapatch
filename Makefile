# 32-bit only. The target is a 2018 MSVC8 x86 PE.
CC      := i686-w64-mingw32-gcc
MH      := ref/minhook
CFLAGS  := -m32 -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu99 \
           -I$(MH)/include -I$(MH)/src -ffunction-sections -fdata-sections
LDFLAGS := -m32 -shared -static-libgcc -Wl,--gc-sections -Wl,--enable-stdcall-fixup

MH_SRC  := $(MH)/src/buffer.c $(MH)/src/hook.c $(MH)/src/trampoline.c $(MH)/src/hde/hde32.c
SRC     := src/dllmain.c src/proxy.c src/hook.c src/sha256.c $(MH_SRC)

all: build/version.dll

build:
	mkdir -p build

build/version.dll: $(SRC) src/version.def | build
	$(CC) $(CFLAGS) $(SRC) src/version.def -o $@ $(LDFLAGS)
	i686-w64-mingw32-objdump -x $@ | grep -A24 "Export Address Table" | head -24

clean:
	rm -rf build
.PHONY: all clean
