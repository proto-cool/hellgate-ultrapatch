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
 * Per-object physics step — carries the HK_TIMER literal "TtStepDelta".
 *
 * NOT hkWorld::stepDeltaTime, which is what this was first labelled. Measured
 * in play it runs ~960 times per 100ms window, roughly 45 calls per frame,
 * so it is per-object (or per-body), not the once-per-frame world step. The
 * float it receives does look like the genuine frame delta; what the counter
 * measures is therefore "physics objects updated", not "steps taken".
 *
 * Called from exactly one place, 0x0049A3EE:
 *
 *     0x49a3e3   fld   dword [ebp + 8]     ; the frame delta, straight from
 *     0x49a3e6   push  ecx                 ; this function's own parameter
 *     0x49a3eb   fstp  dword [esp]
 *     0x49a3ee   call  0x7f92f0
 *
 * There is no loop around it and no fixed timestep: Havok is stepped once
 * per frame with whatever the frame took. Ends `ret 4`, so it is an ordinary
 * callee-cleaned thiscall with one float argument.
 *
 * This is the mechanism H5 turns on. A long frame produces a large delta,
 * a large delta makes every swept body travel further in one step, long
 * sweeps make Havok's internal linear casts walk far more of the MOPP tree,
 * and that makes the next frame longer still.
 */
#define RVA_PHYS_OBJ_STEP       0x003F92F0u
#define RVA_STEP_CALL_SITE      0x0009A3EEu

/*
 * hkWorldCinfo::hkWorldCinfo() — identified by the hkWorldCinfo::vftable
 * store at its head. __fastcall, `this` in ecx, bare `ret`.
 *
 * At 0x0082454E it loads cl=2 and stores it to +0x68 and +0x95. In
 * Havok 4.0, hkWorldCinfo::SimulationType is
 *     INVALID=0, DISCRETE=1, CONTINUOUS=2, MULTITHREADED=3
 * so the game configures CONTINUOUS simulation, i.e. swept (CCD) collision
 * for every moving body, every step. Each sweep against level geometry is a
 * linear cast into the MOPP tree, which is where all the raycast volume
 * comes from. The "2026 fix" patches this byte to 1 (DISCRETE), which stops
 * the stall at the cost of tunnelling everywhere.
 *
 * Corroborated by hkContinuousSimulation and
 * hkSymmetricAgentLinearCast<hkMoppAgent> both being present in the RTTI.
 */
#define RVA_HK_WORLDCINFO_CTOR  0x00424480u

/*
 * hkMoppBvTreeShape::castRay and its collector variant, identified by the
 * HK_TIMER literal "TtrcMopp". Both `ret 0x0c` — ordinary thiscall.
 *
 * These are the RAY-level entry points. queryRayOnTree below them is
 * *recursive* — it descends the tree by calling itself (return addresses
 * 0x871087 and 0x8712ce appear repeatedly in captured stacks), so counting
 * queryRayOnTree calls counts tree-node visits, not rays. Anything reasoning
 * about "number of raycasts" has to be measured here instead.
 */
/*
 * Script action handlers, from the {name, handler} table at 0x00A00F98
 * (.rdata, stride 8, 169 entries — full dump in notes/script-actions.md).
 *
 * All three are __cdecl taking one pointer: the script action context.
 * Verified from the epilogues (bare `ret`, caller cleans) and from
 * SpawnMonster's `mov ebx, dword [ebp + 8]`.
 *
 * The context is a live struct — ctx[2] carries the spawn position at
 * +0x248. Rather than synthesise one, the repro harness *amplifies*: when
 * the game legitimately spawns something, we immediately spawn N more with
 * that same known-good context. No struct layout needs to be understood,
 * and the context cannot go stale because it is reused within the call.
 */
/*
 * The shared spawn primitive that the script actions bottom out in, with
 * eight call sites covering every spawn path. __cdecl, 13 dword arguments
 * (the tightest caller cleanup is `add esp, 0x34`, matching the decompile).
 *
 * Hooking the SpawnObject script action instead produced literally zero
 * calls across a whole session: it is used only by particular skills and
 * quests, not by ordinary spawning. This is the one that actually fires.
 */
#define RVA_SPAWN_PRIMITIVE     0x0021C8F1u

#define RVA_SPAWN_OBJECT        0x0021D108u
#define RVA_SPAWN_MONSTER       0x0021D065u
#define RVA_SPAWN_MONSTER_NEAR  0x0021D1F8u

/*
 * hkWorld::addEntity / ::removeEntity, found via Havok's lock-tag literals
 * "LtAddEntity" / "LtRemEntity" — the same trick as the Tt timer names.
 * Both thiscall, `ret 8`, and both open with `mov esi, ecx`.
 *
 * Every physics body must pass through these, so together they give a live
 * count of bodies in the world. That is precisely H8's independent
 * variable: if continuous collision detection is what generates the ray
 * volume, rays should scale with the number of bodies.
 *
 * It also makes the repro harness unnecessary for *testing* H8 — ordinary
 * play varies the body count on its own, so the correlation can be measured
 * without provoking a stall or destabilising anything.
 *
 * And it is where the real fix goes: per-body hkCollidableQualityType,
 * demoting cosmetic debris while leaving projectiles and the player alone.
 */
#define RVA_HK_ADD_ENTITY       0x003FC630u
#define RVA_HK_REMOVE_ENTITY    0x003FC7E0u

#define RVA_MOPP_CASTRAY        0x00419ED0u
#define RVA_MOPP_CASTRAY_COLL   0x00419F90u
#define HKWORLDCINFO_SIMTYPE    0x95u

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
