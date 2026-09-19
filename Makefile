# 32-bit only. The target is a 2018 MSVC8 x86 PE.
CC      := i686-w64-mingw32-gcc
MH      := ref/minhook
# -fno-omit-frame-pointer is load-bearing, not a debug nicety. x86 has no
# unwind tables, so CaptureStackBackTrace walks the EBP chain. At -O2 GCC
# reuses EBP as a scratch register inside the detours, which destroys the
# chain and makes every captured stack come back empty.
CFLAGS  := -m32 -O2 -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter \
           -std=gnu99 -I$(MH)/include -I$(MH)/src -ffunction-sections -fdata-sections
LDFLAGS := -m32 -shared -static-libgcc -Wl,--gc-sections -Wl,--enable-stdcall-fixup

MH_SRC  := $(MH)/src/buffer.c $(MH)/src/hook.c $(MH)/src/trampoline.c $(MH)/src/hde/hde32.c
SRC     := src/dllmain.c src/proxy.c src/hook.c src/sha256.c $(MH_SRC)

all: build/version.dll build/host.exe

build:
	mkdir -p build

build/version.dll: $(SRC) src/version.def | build
	$(CC) $(CFLAGS) $(SRC) src/version.def -o $@ $(LDFLAGS)
	i686-w64-mingw32-objdump -x $@ | grep -A24 "Export Address Table" | head -24

build/host.exe: test/host.c | build
	$(CC) -m32 -O2 test/host.c -o $@ -lversion

clean:
	rm -rf build
.PHONY: all clean
