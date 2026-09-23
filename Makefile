# 32-bit only. The target is a 2018 MSVC8 x86 PE.
CC      := i686-w64-mingw32-gcc
MH      := ref/minhook
# -fno-omit-frame-pointer is load-bearing, not a debug nicety. x86 has no
# unwind tables, so CaptureStackBackTrace walks the EBP chain. At -O2 GCC
# reuses EBP as a scratch register inside the detours, which destroys the
# chain and makes every captured stack come back empty.
# The version is the commit count: it goes up with every commit, no bumping.
HG_VERSION := 0.$(shell git rev-list --count HEAD 2>/dev/null || echo 0)
HG_COMMIT  := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)$(shell git diff --quiet HEAD 2>/dev/null || echo +)
CFLAGS  := -DHG_VERSION='"$(HG_VERSION)"' -DHG_COMMIT='"$(HG_COMMIT)"' -m32 -O2 -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter \
           -std=gnu99 -I$(MH)/include -I$(MH)/src -ffunction-sections -fdata-sections
LDFLAGS := -m32 -shared -static-libgcc -Wl,--gc-sections -Wl,--enable-stdcall-fixup -lpsapi -lws2_32 -lwinmm

MH_SRC  := $(MH)/src/buffer.c $(MH)/src/hook.c $(MH)/src/trampoline.c $(MH)/src/hde/hde32.c
SRC     := src/dllmain.c src/proxy.c src/hook.c src/panel.c src/overlay.c src/device.c src/postfx.c src/compare.c src/brand.c src/plshadow.c src/volfog.c src/crashlog.c \
           src/ui.c src/panel_ui.c src/fart.c src/shoulder.c src/altlatch.c src/gfxprobe.c src/sha256.c $(MH_SRC)

# The UI core is plain C with no Windows or D3D dependency, so its tests
# build and run natively. That is the point of the split: the panel's layout
# and hit testing can be iterated on without a Proton launch.
HOSTCC  ?= cc

# Where the DLL has to land to be testable. Override GAME for another install.
GAME    ?= $(HOME)/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/HELLGATE_London

# `all` deploys. A DLL sitting in build/ is not testable, and a stale one in
# the game directory is worse than none: it looks like the change did not
# work. Building and installing are therefore the same step.
all: build/version.dll build/host.exe build/vtable.exe build/fxdis.exe build/uitest install

build:
	mkdir -p build

# rewritten only when the version or commit changes, so a commit rebuilds the DLL
build/hgver.txt: FORCE | build
	@echo "$(HG_VERSION) $(HG_COMMIT)" | cmp -s - $@ || echo "$(HG_VERSION) $(HG_COMMIT)" > $@
FORCE:

build/version.dll: $(SRC) src/fxtable.h src/version.def build/hgver.txt | build
	$(CC) $(CFLAGS) $(SRC) src/version.def -o $@ $(LDFLAGS)
	i686-w64-mingw32-objdump -x $@ | grep -A24 "Export Address Table" | head -24

build/host.exe: test/host.c | build
	$(CC) -m32 -O2 test/host.c -o $@ -lversion

# Behavioural check on the D3D9 vtable slot indices the overlay hooks.
build/vtable.exe: test/vtable.c | build
	$(CC) -m32 -O2 test/vtable.c -o $@ -ld3d9

# Shader tooling, all run under Wine in the dev toolbox (see the tools/fx/*.c headers):
#   fxdis   disassemble SM1-3 blobs (with Wine's d3dx9_43)
#   fxcc    compile one HLSL entry point (vkd3d, or FXCC_DLL=<d3dx9_34.dll>)
#   fxcomp  compile an .fx with Microsoft's effect compiler (d3dx9_34) -- what ships
#   fxload  load and validate an .fxo with the game's own D3DX before the game does
#   fxdiff  draw every technique of two effects with the same inputs and compare
# `tools/fx/build_shaders.sh` chains them into the override effects.
build/fxdis.exe: tools/fx/fxdis.c | build
	$(CC) -m32 -O2 tools/fx/fxdis.c -o $@
build/fxcc.exe: tools/fx/fxcc.c | build
	$(CC) -m32 -O2 tools/fx/fxcc.c -o $@
build/fxcomp.exe: tools/fx/fxcomp.c | build
	$(CC) -m32 -O2 tools/fx/fxcomp.c -o $@
build/fxload.exe: tools/fx/fxload.c | build
	$(CC) -m32 -O2 tools/fx/fxload.c -o $@ -ld3d9
build/fxdiff.exe: tools/fx/fxdiff.c | build
	$(CC) -m32 -O2 -Wall tools/fx/fxdiff.c -o $@ -ld3d9
# Build our material effects and install them to $(GAME)/override (docs/graphics.md).
shaders:
	toolbox run -c dev tools/fx/build_shaders.sh
# Parity of our material shaders with stock, all six effects.
MATPAIRS := actoroutdoor30:actor actorindoor30:actor backgroundoutdoor30:background \
            backgroundindoor30:background backgroundoutdoorprop30:background backgroundindoorprop30:background
matcheck:
	toolbox run -c dev tools/fx/matcompile.sh $(MATPAIRS)
	for p in $(MATPAIRS); do \
	    echo "$${p%%:*}"; toolbox run -c dev tools/fx/matcheck.sh $${p%%:*} $${p##*:} | grep -a '^seed' || exit 1; done

# Behavioural check on the panel's layout and hit testing. Runs anywhere.
build/uitest: test/ui.c src/ui.c src/ui.h src/panel_ui.c src/panel_ui.h src/panel.h src/fart.c src/shoulder.c src/altlatch.c | build
	$(HOSTCC) -O1 -g -Wall -Wextra -Wno-unused-parameter -std=gnu99 \
	    test/ui.c -o $@ -lm

test: build/uitest
	./build/uitest

# Render a fart to a file so the sound can be judged without the game.
fart: build/uitest
	./build/uitest --fart build/fart.wav

install: build/version.dll
	@if [ -d "$(GAME)/bin" ]; then \
	    cp build/version.dll "$(GAME)/bin/version.dll" && \
	    echo "installed -> $(GAME)/bin/version.dll"; \
	    python3 tools/launcher.py patch "$(GAME)"; \
	else \
	    echo "SKIPPED install: no $(GAME)/bin (set GAME=... to point at the install)"; \
	fi

# The DLL and override/ are all we add. The one game file we change is the
# launcher (tools/launcher.py: no dialog), and uninstall puts its bytes back.
uninstall:
	rm -f "$(GAME)/bin/version.dll"
	python3 tools/launcher.py restore "$(GAME)"
	rm -rf "$(GAME)/override"
	@echo "removed -> $(GAME)/bin/version.dll and $(GAME)/override"

# Symbol recovery (docs/codemap). Needs a Ghidra project of the executable
# (~/ghidra_proj/HG.gpr, program hg_sp.exe) and Java in the dev toolbox:
#   toolbox run -c dev make codemap
#   toolbox run -c dev make decomp F="dxC_EffectGetTechniqueByFeatures 0x78078f"
GHIDRA ?= $(HOME)/opt/ghidra_12.1.3_PUBLIC/support/analyzeHeadless
GPROJ  ?= $(HOME)/ghidra_proj
codemap:
	$(GHIDRA) $(GPROJ) HG -process hg_sp.exe -noanalysis -scriptPath tools/re/ghidra \
	    -postScript NameFromAsserts.java $(CURDIR)/docs/codemap/functions.csv 2>&1 | grep "java>" || true
	python3 tools/re/codemap.py

decomp:
	$(GHIDRA) $(GPROJ) HG -process hg_sp.exe -noanalysis -readOnly -scriptPath tools/re/ghidra \
	    -postScript Decomp.java $(CURDIR)/decomp $(F) 2>&1 | grep "java>" || true

clean:
	rm -rf build
.PHONY: all clean test install uninstall fart codemap decomp shaders matcheck FORCE
