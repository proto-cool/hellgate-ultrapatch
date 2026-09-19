# Target binary facts (pin everything to this)

| Field | Value |
|---|---|
| Path | `<steamlib>/steamapps/common/HELLGATE_London/bin/Hellgate_sp_x86.exe` |
| sha256 | `401e011de53583c2a7d7971ee8981ed59796b1a672d353d790b9195a20863dc3` |
| Size | 11,345,920 bytes |
| PE timestamp | 2018-11-27 22:38:17 UTC |
| Linker | MSVC 8.0 (VS2005) |
| ImageBase | 0x00400000 |
| Relocations | **STRIPPED** → image always loads at 0x400000, VA == RVA+0x400000 |
| Large address aware | yes |
| Entry point | RVA 0x00503e09 |
| SizeOfImage | 0x00e82000 |
| PDB path | `f:\P_2017\URC\release\Hellgate_sp_x86.pdb` |
| Steam appid / buildid | 939520 / 3392971 |

Launcher `Hellgate.exe` sha256 `66d3898c184e730717028613ad8c361b6597dcd730af4f2ebc866f7ca04e1e94`.

## Imported DLLs
KERNEL32, USER32, SHELL32, **VERSION**, WS2_32, granny2, binkw32, mss32, RPCRT4,
SHLWAPI, OLEACC, GDI32, WINSPOOL.DRV, comdlg32, ADVAPI32, ole32, OLEAUT32,
fmodex, **DINPUT8**, d3dx9_34, umbra, d3d9, WININET, dbghelp.

- No anti-cheat imports. `dbghelp` is for crash dumps.
- Renderer is **D3D9** (confirmed) + `d3dx9_34`. Do NOT proxy `d3d9.dll` (DXVK owns it).
- **Proxy candidate chosen: `VERSION.dll`** — tiny export surface, imported directly,
  no Proton component owns it. `DINPUT8.dll` is the backup.

## Havok
- **Havok 4.0** — SDK path strings `C:\prime\3rd Party\Havok40\sdk\include\...`.
- Class prefix is `hk*`, **not** `hkp*` (the `hkp` prefix arrived in Havok 4.5/5.x).
  Augmentrex's writeup says `hkpMoppLongRayVirtualMachine`; in this binary it is
  `hkMoppLongRayVirtualMachine`.
- Statically linked — no Havok DLL ships with the game.
- Retained `__FILE__` assert strings, e.g. `.\collide\mopp\machine\hkMoppLongRayVirtualMachine.cpp`.
- MSVC RTTI present (~385 `.?AV` type descriptors) → class-name recovery is available.
- Built-in rdtsc profiler with TLS timing blocks; 4-char tags incl. `TtMopp`,
  `TtrcMopp`, `TtCapsCaps` (the latter is augmentrex's capsule-capsule anchor).

## Addresses established so far
| VA | What | How found |
|---|---|---|
| 0x00870B10 | `hkMoppLongRayVirtualMachine::queryRayOnTree` | augmentrex byte pattern, **unique** match in this build |
| 0x00870F0C, 0x0087110F, 0x00871CE0, 0x00871E50 | its 4 direct callers | E8 rel32 scan |
| 0x00819ED0, 0x00819F90 | callers of 0x871CE0 / 0x871E50; both are entries in a Havok shape-type dispatch table | E8 scan + abs-ptr scan |
| 0x009AB62C.. | Havok shape-type dispatch table containing the above | abs-ptr scan |
