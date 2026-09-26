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
 * CORRECTION (2026-09-21): this is hkAnimatedSkeleton::stepDeltaTime, not
 * physics. It sits among hkAnimatedSkeleton's methods (ctor 0x7f93e0,
 * add/removeAnimationControl 0x7f9380 / 0x7f9220, sampler 0x7f9610) and
 * walks the skeleton's control list at +0xc/+0x10. So "~45 calls per
 * window" below was counting animated characters being advanced. The name
 * RVA_PHYS_OBJ_STEP is kept so existing code and logs still line up.
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
 * (.rdata, stride 8, 169 entries — full dump in docs/reference/script-actions.md).
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

/*
 * Local player unit resolver, found from the "Marcus Fidelius" default-name
 * sites (0x004c1f0e, 0x00517f18, 0x0055e5b8). __cdecl, no arguments, returns
 * the local player's unit pointer or NULL. 51 direct callers.
 *
 *     0x404e04  push ecx
 *     0x404e05  mov  eax, ds:0xb96dec     ; cached unit id, -1 until resolved
 *     0x404e0a  cmp  eax, 0xffffffff
 *     0x404e0d  jne  0x404e32
 *     ...       call 0x4214ed("default")  ; resolve once, cache it
 *     0x404e32  mov  edx, eax
 *     0x404e36  call 0x4213c2             ; id -> unit pointer
 *
 * Two unit-struct offsets fall straight out of those same call sites:
 *   +0x110  flags word; bit 0x100 tested at 0x0049cfd0
 *   +0x120  wide name, 0x20 chars, memcpy'd at 0x00517f09 / 0x0055e5ae
 * Everything else in the unit is still unmapped — that is what the panel's
 * peek view is for.
 *
 * NOTE: 0x4213c2 takes its argument in edx with eax zeroed, which is not a
 * calling convention GCC can express. Do not call it directly; always go
 * through 0x404e04, which is a plain __cdecl wrapper around it.
 */
#define RVA_GET_LOCAL_PLAYER    0x00004E04u
#define RVA_LOCAL_PLAYER_ID     0x00796DECu
#define UNIT_OFF_FLAGS          0x110u
#define UNIT_OFF_NAME           0x120u
#define UNIT_NAME_CHARS         0x20u

/*
 * Shift+` -- CMD_CONSOLE_TOGGLE's own binding, recovered from the keybind
 * table at 0x00b9ed48 (entry base 0x00b9ff70, stride 0x38).
 *
 * Both the key at +0x14 and the modifier at +0x18 are shifted one entry: the
 * values stored in entry N belong to entry N+1. With the shift applied
 * CMD_MOVE_LEFT/RIGHT/FORWARD decode as A/D/W and CMD_HOTSPELL_1 as F1;
 * without it, D/W/S and F2.
 *
 * Reading +0x18 straight out of the console entry gives 0, which is how an
 * earlier pass here concluded the console was on a bare `. It is not: the
 * 0x10 (SHIFT) sits one entry earlier. A bare ` opens the chatbox, which is
 * live code and must not be stolen -- only the Shift+` console is dead.
 *
 * ..\consolecmd.cpp is down to a single surviving assert, so that binding
 * does nothing in this build. The overlay takes it over.
 */
#define KEY_CONSOLE_TOGGLE      0xC0u   /* VK_OEM_3 */
#define KEY_CONSOLE_NEEDS_SHIFT 1

/*
 * First person, and the gate that keeps melee weapons out of it.
 *
 * First person is a fully live camera mode in this build -- mode 0, against
 * 6 for third person -- with named engine handlers (FirstPersonCamera at
 * 0x0052c8db, ThirdPersonCamera at 0x0052c900) and real support downstream:
 * six `mode == 0` sites doing aim and projection work, a dedicated
 * first-person branch in the camera update at 0x004da0fa, a separate
 * "first person" appearance group, MODEL_FLAGBIT_FIRST_PERSON_PROJ, and
 * first-person jump/land footstep columns.
 *
 * It is the *weapon* that forbids it, in two places:
 *
 *   1. Inside SetCameraMode itself, at 0x004dc095-0x004dc0d5. After the
 *      requested mode is in ebx it fetches the items in weapon slots 7 and
 *      8 and asks CanUseFirstPerson about each; if either says no, ebx is
 *      rewritten to 6. So *every* request is overridden, including the
 *      engine's own FirstPersonCamera event.
 *
 *   2. On skill start, at 0x0062b31b. If the camera is in first person and
 *      the same two flags are set, it calls SetCameraMode(6, 0) -- so even
 *      past the first gate, swinging would throw you back out.
 *
 * CanUseFirstPerson is a clean standalone predicate:
 *
 *     int __cdecl CanUseFirstPerson(void *pItem)
 *     {
 *         if (UnitTestFlag(pItem, 0x2c)) return 0;
 *         if (UnitTestFlag(pItem, 0x29)) return 0;
 *         return 1;
 *     }
 *
 * "UnitTestFlag" (0x45a6bd) is really UnitIsA(item, unittype): it takes
 * pItem->0x340, the item's unit type, and asks whether it is one of the
 * given type; 0x2c is unittypes row 44, melee, and 0x29 row 41, shield
 * (the script's getWieldingIsACount calls it the same way). So it is the
 * weapon's type that forbids first person -- which is why the behaviour
 * follows the weapon and not the class. It has exactly two callers, both the slot checks above, so
 * detouring it to return 1 is precisely scoped: nothing else in the image
 * asks that question.
 */
#define RVA_SET_CAMERA_MODE     0x000DBFDAu  /* __cdecl (int mode, int force) */
#define RVA_RESTORE_CAMERA      0x000DBF9Cu  /* __cdecl (void)                */
#define RVA_CAN_FIRST_PERSON    0x000DBFB5u  /* __cdecl (void *item) -> bool   */
#define RVA_CAMERA_MODE_CUR     0x006DE538u  /* the live mode, read-only here  */

/*
 * The `jne` at 0x0062b322 that guards the skill-start kick. Turning it into
 * an unconditional `jmp` (0x75 -> 0xeb, same displacement) skips the whole
 * force-to-third-person block. One byte, restored on toggle off.
 */
#define RVA_SKILL_FP_KICK       0x0022B322u
#define SKILL_FP_KICK_JNE       0x75u
#define SKILL_FP_KICK_JMP       0xEBu

#define CAM_FIRST_PERSON        0
#define CAM_THIRD_PERSON        6

/*
 * The camera, for the over-the-shoulder offset (src/shoulder.c).
 *
 * CameraUpdate is a plain __cdecl (void *game), called once per frame from
 * 0x4dc79f (and from a handful of mode-change sites). It computes the whole
 * camera into CAMERA_INFO, which CameraGetInfo (0x4dbf84) returns to the
 * renderer and to picking -- it hands back 0xf6f178 unless a detached camera
 * is active. In the third-person branch (mode 6, at 0x4da41c):
 *
 *   look-at = player + 0.2 * (cos yaw, sin yaw) + head height
 *   eye     = player + dist * (cos(yaw+pi), sin(yaw+pi)),  z -= dist*sin(pitch)
 *
 * with yaw/pitch at CAMERA_INFO+0x30/+0x34 and dist the zoom distance at
 * 0xba1380, eased toward its target at 0xba137c (1.75 stock). The game's
 * camera collision ray runs last, at 0x4dbe74, so a post-hook sees the final
 * eye.
 */
#define RVA_CAMERA_UPDATE       0x000D9EFAu  /* __cdecl (void *game)          */
#define RVA_CAMERA_INFO         0x00B6F178u  /* CAMERA_INFO, 0xf6f178          */
#define CAMINFO_EYE             0x00u        /* float[3]                      */
#define CAMINFO_LOOKAT          0x0Cu        /* float[3]                      */
#define CAMINFO_PITCH           0x34u        /* float, radians, wraps 0..2pi;
                                              * copied from the view pitch at
                                              * 0xba14ec, which mouse look
                                              * clamps to +-85 deg (0x50e035) */
#define RVA_CAMERA_DIST         0x007A1380u  /* float, smoothed zoom distance */

/*
 * The game's camera collision cast, and the chain to the world it needs.
 *
 * 0x587ebd: origin in ecx, unit direction in eax, stack (hkWorld *world,
 * float length, int flags), caller cleans, returns the free distance along
 * the ray in xmm0 (length when nothing is hit, 0 when world is null). The
 * camera reaches it through 0x5d348c with flags 0.
 *
 * The world is level->+0xb8, where CameraUpdate gets the level from its
 * game argument: unit = game->+0x238 (only when game->+0x14 is 0, i.e. the
 * client), room = unit->+0x2c, level = room->+0x130.
 */
#define RVA_CAMERA_RAY          0x00187EBDu
#define GAME_IS_SERVER          0x14u
#define GAME_CONTROL_UNIT       0x238u  /* the camera's unit (0x505e09)             */
#define GAME_PLAYER_UNIT        0x23Cu  /* the skill code's "you" (0x435ee7)     */
#define UNIT_ROOM               0x2Cu
#define ROOM_LEVEL              0x130u
#define LEVEL_HKWORLD           0xB8u

/*
 * Third-person zoom ceiling. In the wheel handler (0x4dc34f) the target
 * distance steps by 0.5 and is clamped to [0xade558 = 1.5, 5.0], the 5.0
 * read by this instruction from a constant 102 other sites share. Only the
 * instruction's operand is repointed.
 */
#define RVA_ZOOM_MAX_INSN       0x000DC3A8u  /* movss xmm2,[0xa0086c]         */

/*
 * Skill start, for the melee camera impulse. At 0x62b31b the skill-start
 * code has its context in ebx -- unit at +0x4, weapon item at +0x8 -- and
 * the block that follows asks UnitTestFlag(weapon, 0x2c / 0x29), the melee
 * flags, before kicking the camera out of first person. The hook sits on
 * the 7-byte `cmp [0xc48e9c],2` there; the `jne` right after it (0x62b322)
 * is RVA_SKILL_FP_KICK, so the two never touch the same bytes.
 *
 * UnitTestFlag: item in eax, flag on the stack, caller cleans, eax result.
 */
#define RVA_SKILL_START_SITE    0x0022B31Bu
#define SKILLCTX_UNIT           0x04u
#define SKILLCTX_WEAPON         0x08u
#define RVA_UNIT_TEST_FLAG      0x0005A6BDu
#define ITEMFLAG_NO_FP_A        0x2C
#define ITEMFLAG_NO_FP_B        0x29

/*
 * Animation (Havok Animation 4.0, statically linked; Granny is loaded but
 * this build never takes its branch).
 *
 * 0x490d9a is the game's animation trace: cdecl (const char *event, model),
 * with the animation record in esi (or 0). It prints
 *   "%f %s ID:%d File:%s Model:%d(%s) WGrp:%s Pri:%d Group:%s"
 * only behind the debug flag at 0xededb4 and only for the debug unit, but
 * is called for every animation event regardless.
 *
 * Ease-in and ease-out are hkDefaultAnimationControl's, inlined as plain
 * functions: control in eax, duration on the stack, ret 4. Control +0x60
 * is 1/duration (FLT_MAX when the duration is ~0), +0x64 the ease position,
 * +0x68 1 while easing in.
 */
#define RVA_ANIM_TRACE          0x00090D9Au
#define RVA_HK_EASE_IN          0x000910A9u
#define RVA_HK_EASE_OUT         0x0009CBBAu
#define ANIMREC_CONTROL         0x18u        /* hkDefaultAnimationControl *   */
#define HKCTL_LOCAL_TIME        0x08u        /* hkAnimationControl::m_localTime;
                                              * confirmed by a layout dump in
                                              * game: rises, wraps at the cycle */
#define HKCTL_BINDING           0x1Cu        /* hkAnimationBinding *            */
#define HKCTL_MASTER_WEIGHT     0x2Cu        /* what 0x49359a zeroes            */
#define HKCTL_EASE_INV_DUR      0x60u
#define HKCTL_EASE_T            0x64u
#define HKCTL_EASE_STATUS       0x68u        /* byte: 1 easing in, 0 out        */
#define HKBIND_ANIM             0x08u        /* Havok 4.0 layout -- ASSUMED     */
#define HKANIM_DURATION         0x0Cu        /* Havok 4.0 layout -- ASSUMED     */

/*
 * hkAnimatedSkeleton (vtable 0x9a632c, ctor 0x7f93e0). The sampler is
 * 0x7f9610, thiscall (pose, nbones, cache, flag), ret 0x10, reached through
 * virtual slots 2 and 3 (0x7fa220 / 0x7fa240). pose is nbones hkQsTransform
 * (translation, rotation xyzw, scale; 48 bytes each). Controls live at
 * +0xc (pointer) / +0x10 (count); the skeleton at +0x18, bone count +0x10.
 *
 * 0x49359a is the UPDATE_WEIGHTS pass's instant cut:
 *     movss [eax+0x2c], xmm1     ; control master weight := 0
 */
#define RVA_HK_SAMPLE           0x003F9610u
#define HKSKEL_CONTROLS         0x0Cu
#define HKSKEL_NCONTROLS        0x10u
#define RVA_STANCE_ZERO_STORE   0x0009359Au
#define ANIMREC_DEF             0x1Cu
#define ANIMDEF_FILE            0x0Cu        /* char[], inline                */
#define ANIMDEF_EASE_IN         0x134u       /* float; 0 -> 0.1 at 0x4934fd   */
#define ANIMDEF_EASE_OUT        0x138u       /* float; passed through as-is   */
#define MODEL_OWNER_ID          0x18u
#define UNIT_ID                 0x2DCu
#define RVA_GAME_GLOBAL         0x00B267A4u  /* the game, 0xf267a4            */

/*
 * Viewmodel surface, for the melee first-person problem.
 *
 * With the camera unlocked there is nothing to look at: a unit carries two
 * models, and first person draws only the first-person one. Gun appearances
 * have entries in the "first person" appearance group; melee weapons do
 * not, so the view is empty. The plan is to draw the *third*-person model
 * in first person and hide the head, which is why these are here.
 *
 * Recovered from the setup code at 0x004d31b0, which is the only place that
 * touches both models together:
 *
 *     0x4d31f7  push 1 ; push 7 ; mov esi,ecx ; call 0x78e20f
 *               -> e_ModelSetFlagbit(nModelFirst, FIRST_PERSON_PROJ, 1)
 *     0x4d3224  push 0 ; push 7 ; mov [esp+14],ecx ; call 0x78e20f
 *               -> e_ModelSetFlagbit(nModelThird, FIRST_PERSON_PROJ, 0)
 *
 * so bit 7 is MODEL_FLAGBIT_FIRST_PERSON_PROJ, and the third-person model
 * is explicitly told not to use it. MODEL_FLAGBIT_NODRAW exists too (the
 * name survives in dxC asserts) but its bit number has not been recovered,
 * which is exactly why the panel can poke an arbitrary bit: finding it
 * takes one glance in game and a great deal of disassembly otherwise.
 *
 * CONVENTIONS, both unusual and both hand-shimmed in hook.c:
 *   c_UnitGetModelIdThirdPerson  arg in EAX, no stack args, returns EAX.
 *   e_ModelSetFlagbit            arg1 in ECX, two CALLER-cleaned stack args.
 */
#define RVA_UNIT_MODEL_THIRD    0x00035F46u
#define RVA_MODEL_SET_FLAGBIT   0x0038E20Fu
#define MODEL_FLAGBIT_FP_PROJ   7

#endif

/*
 * Model flag bits (e_ModelSetFlagbit(nModelId, bit, on)), read from the
 * assert sites that name them: FIRST_PERSON_PROJ = 7 (0x4d31b0),
 * NOSHADOW = 4 (the paperdoll setter, 0x4b9236: "e_ModelSetFlagbit(
 * pUnit->pGfx->nModelIdPaperdoll, MODEL_FLAGBIT_NOSHADOW, 1 )").
 * The shadow-map draw list skips models with NOSHADOW; whether the
 * player's third-person model carries it is the open question for
 * "the player casts no shadow" (LOG 2026-09-21).
 */
#define MODEL_FLAGBIT_NOSHADOW  4
