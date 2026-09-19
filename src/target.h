#ifndef HG_TARGET_H
#define HG_TARGET_H

/*
 * Everything here is pinned to one exact binary. If the hash does not match,
 * the DLL refuses to hook: every address below would otherwise be a wild
 * write into someone else's code.
 *
 *   Hellgate_sp_x86.exe, Steam appid 939520, buildid 3392971
 *   PE timestamp 2018-11-27 22:38:17 UTC, linker MSVC 8.0
 *   Relocations are STRIPPED, so the image always loads at 0x00400000
 *   and VA == ImageBase + RVA with ImageBase fixed.
 */
#define HG_EXE_SHA256 "401e011de53583c2a7d7971ee8981ed59796b1a672d353d790b9195a20863dc3"
#define HG_EXE_SIZE   11345920u
#define HG_IMAGE_BASE 0x00400000u

/* hkMoppLongRayVirtualMachine::queryRayOnTree — unique byte-pattern match. */
#define RVA_QUERY_RAY_ON_TREE   0x00470B10u

/* hkWorld::castRay — identified by its HK_TIMER literal "TtRayCstCached". */
#define RVA_HK_WORLD_CAST_RAY   0x00441800u

/* Game-side raycast wrapper: the single direct caller of hkWorldRayCaster. */
#define RVA_GAME_RAYCAST_WRAPPER 0x003FB9E0u

/*
 * The game's central world-raycast helper, and the choke point this whole
 * investigation turns on. Recovered ABI (verified against the disassembly,
 * not just the decompiler):
 *
 *   __fastcall FUN_005d30df(void *ecx, void *edx,
 *                           const float *origin,   [ebp+0x08]
 *                           const float *dir,      [ebp+0x0c]
 *                           float        length,   [ebp+0x10]
 *                           unsigned     a6,       [ebp+0x14]
 *                           unsigned     a7)       [ebp+0x18]
 *
 * It builds an hkWorldRayCastInput on the stack as
 *     from = origin
 *     to   = origin + dir * length
 * and casts it. `length` is a single scalar supplied by the caller: if it is
 * garbage, `to` is garbage and hkMoppLongRayVirtualMachine walks the whole
 * MOPP tree. That is hypothesis H2's exact mechanism.
 *
 * ecx and edx are both null-checked before any work happens.
 */
#define RVA_GAME_RAYCAST        0x001D30DFu

/*
 * hkWorldRayCastInput layout, recovered twice independently (from the
 * hkWorldRayCaster decompilation and from the stack frame the game helper
 * builds):
 *     +0x00  hkVector4 m_from
 *     +0x10  hkVector4 m_to
 *     +0x20  hkBool    m_enableShapeCollectionFilter
 *     +0x24  hkUint32  m_filterInfo
 */

#endif
