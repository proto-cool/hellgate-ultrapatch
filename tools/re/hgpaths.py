"""Shared inputs for the reverse-engineering scripts in tools/re.

HG_EXE        the game executable (default: the Flatpak Steam install)
HG_CALLGRAPH  call graph pickle written by callgraph.py (default:
              build/callgraph.pkl; make it with
              `python3 tools/re/callgraph.py "$HG_EXE" build/callgraph.pkl`)
"""
import os

EXE = os.environ.get("HG_EXE") or os.path.expanduser(
    "~/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/"
    "HELLGATE_London/bin/Hellgate_sp_x86.exe")
CALLGRAPH = os.environ.get("HG_CALLGRAPH") or "build/callgraph.pkl"
