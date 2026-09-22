#!/bin/bash
# The 64-bit render server (docs/graphics-plan.md): NVIDIA's RTX Remix
# bridge (MIT) forwards every D3D9 call from the 32-bit game to a 64-bit
# process, which renders with Proton's own 64-bit DXVK instead of the Remix
# path tracer. Textures, shaders and the Vulkan driver then live outside the
# game's 4 GB.
#
#   tools/bridge.sh on     install into <game>/bin (downloads once into build/bridge)
#   tools/bridge.sh off    remove it; the game renders in-process again
#
# The gh download needs the dev toolbox: toolbox run -c dev tools/bridge.sh on
set -e
cd "$(dirname "$0")/.."
GAME=${GAME:-$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/HELLGATE_London}
PROTON=${PROTON:-$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Proton - Experimental}
REL=remix-1.5.2
B=build/bridge
BIN=$GAME/bin

case "$1" in
on)
    if [ ! -f "$B/client/d3d9.dll" ]; then
        mkdir -p "$B/client" "$B/server"
        [ -f "$B/$REL-release.zip" ] || gh release download "$REL" -R NVIDIAGameWorks/rtx-remix \
            -p "$REL-release.zip" -D "$B"
        unzip -o -q -j "$B/$REL-release.zip" d3d9.dll LICENSE.txt -d "$B/client"
        unzip -o -q -j "$B/$REL-release.zip" .trex/NvRemixBridge.exe -d "$B/server"
    fi
    cp "$B/client/d3d9.dll" "$BIN/d3d9.dll"
    mkdir -p "$BIN/.trex"
    cp "$B/server/NvRemixBridge.exe" "$BIN/.trex/"
    cp "$PROTON/files/lib/wine/dxvk/x86_64-windows/d3d9.dll" "$BIN/.trex/d3d9.dll"
    echo "bridge on: $BIN/d3d9.dll (32-bit client) -> .trex/NvRemixBridge.exe + 64-bit DXVK"
    ;;
off)
    rm -f "$BIN/d3d9.dll"
    rm -rf "$BIN/.trex"
    echo "bridge off"
    ;;
*)
    echo "usage: $0 on|off" >&2; exit 2 ;;
esac
