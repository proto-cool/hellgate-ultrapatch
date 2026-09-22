<!--
Saved 2026-09-22 at the user's request: a web research write-up on community
complaints and existing fixes for the Steam build, covering the whole
ultrapatch effort. Kept as received. Its claims come from community sources
and have not been checked against this repo's findings. Two known differences:
this repo traced the 1 FPS bug to continuous collision detection driving
the MOPP ray machine (README / docs/reference/russian-patch-analysis.md), which is more
specific than the "raycast storm" framing below; and DXVK is now the default
renderer here (docs/graphics-plan.md).
-->

# Hellgate: London (Steam, 2018) — Community Complaints, Fixes, and Patch-Feasibility Report

## TL;DR
- The single most-reported, most-damaging problem by far is the **"1 FPS bug"** — a Havok raycast stall the user is already targeting; after that, the highest-value unaddressed fixes are **inventory/stash management (auto-sort, bigger stash, item comparison), loot/HUD clutter, and restoring the bugged Transmogrifying Cube + endgame drops** that gate a huge amount of content.
- Most **data/content problems are moddable** on the Steam 2.1.0.4 client via the Reanimator-steam toolchain (Contra's and Mayhem's mods prove it), but **UI layout, HUD scaling, an inventory auto-sort button, and enlarged inventory/stash grids appear hardcoded in the executable** and are the risky, high-effort items.
- The Steam build is a port of the Korean "Hellgate Global / Tokyo 2.0" client and is widely regarded as a **regression** from the 2007 retail disc: no multiplayer, no DX10, no 64-bit exe, lower-res textures/cutscenes, bad English localization, and a large amount of bugged/inaccessible endgame content.

## Key Findings

### Severity / frequency ranking (from Steam reviews, forums, Reddit, PCGamingWiki)
1. **1 FPS / frame-stall bug** — the dominant complaint; appears in reviews, pinned threads, and mod projects across 2018–2026. Described as "unplayable," "FPS slideshow every couple of minutes."
2. **Missing/bugged endgame content** (Transmogrifying Cube, Stonehenge/Base Defense drops, Berial re-fight, Cow Level) — very frequent among returning veterans.
3. **Inventory/stash limits + no QoL** (no shared stash, tiny stash, no auto-sort, clutter) — very frequent.
4. **Regression vs. 2007 retail** (no MP, no DX10/64-bit, worse textures/cutscenes) — very frequent.
5. **Bad English localization** — frequent.
6. **Crashes / save loss** — moderate but severe when it hits.
7. **Widescreen/ultrawide/FOV, high-refresh, controller** — moderate, mostly among enthusiasts.

The game currently sits at **"Mixed"** on Steam: **52% of 1,125 user reviews are positive** per the Steam store page ("Mixed (1,125) - 52% of the 1,125 user reviews for this game are positive"), and Steambase reports a **Player Score of 57/100 ("Mixed") across 2,308 total reviews**. It remains a niche, low-population title — Steambase shows only **19 concurrent players** against a prior peak of 51 (3/21/2026) — reflecting a genuinely fun ARPG buried under technical and content problems and long since abandoned by its publisher.

## Details

### A. Technical bugs

**1. The "1 FPS" bug (the marquee issue).**
Confirmed root cause, per alexrp's Augmentrex GitHub README (author "Zor"): the game issues an excessive number of Havok ray-cast queries under certain conditions, stalling the frame rate to ~1 FPS. Augmentrex fixes it via `patch-long-ray-vm`, described in the README as: "Disables the game's ray casting engine, fixing the vast majority of frame rate issues (commonly known as the 1 FPS bug)… done by patching the `hkpMoppLongRayVirtualMachine::queryRayOnTree()` function from the Havok Physics library… so that it simply returns rather than running the bytecode passed to it." A second command, `patch-cc-agent`, disables the "capsule-capsule collision agent," fixing frame-rate drops for certain skills (e.g., Blademaster's Whirlwind). Community folk-wisdom links triggers to death/corpse animations and particle/smoke effects on crowded maps ("physics are not able to find a good position for corpses"), and elite/boss deaths.

Reported triggers: dense enemy packs, boss/elite deaths, corpse physics, fire/AoE spell effects, entering certain zones (e.g., the Tokyo portal at Charing Cross), and it recurs indefinitely.

Known community workarounds (all partial):
- **Augmentrex** (alexrp) — in-memory EasyHook injector; `patch-long-ray-vm` + `patch-cc-agent`. Best-known fix. Side effects documented in the README: see nameplates/portals through terrain, ranged collision breaks (shoot through walls), corpses vanish, and **certain bosses (Ash, Oculis) rely on raycasting and cannot be killed unless you toggle the patch off** for those fights (the README recommends a shift+F1 toggle via `key --add -s F1 patch-long-ray-vm`). Requires .NET 4.7.2; some users need a compatibility-mode workaround for an EasyHook `STATUS_INTERNAL_ERROR`.
- **DXVK / Vulkan** — drop the 32-bit `d3d9.dll` from DXVK into `\bin`; several 2025 reports say it eliminates or drastically reduces the bug. Note: DXVK ≥2.7 needs GPU drivers with `VK_KHR_maintenance5`.
- **NVIDIA Control Panel tweaks** (Prefer max performance, VSync off, Low Latency On/Ultra, Threaded Optimization on) — reduces but doesn't eliminate.
- **Launch `Hellgate_sp_x86.exe` from `\bin` directly**, lower "detail of spell effects," set Texture quality to Medium — partial.
- **Outrun it** (sprint/adrenaline ~150–200 m) — temporary.
- A newer Russian "binary patch" mod (on Boosty) claims a binary FPS-drop fix plus a fix for a radar-device location-change crash.

The user's ultrapatch already attempts this — the key insight for him is that Augmentrex's raycast-disable is a *symptom* treatment with real gameplay side effects (boss fights, through-wall combat), so a more surgical fix (only disabling the pathological query pattern, or keeping raycasting for boss/aiming logic) would be a meaningful improvement over the existing tools.

**2. Crashes.** Reported on Summoner ("Constant crashes on Summoner"), when killing many enemies or picking up many items, on exit (hard-crashing the PC), and a radar-device crash on zone change. Log files can balloon to gigabytes; a known mitigation is setting `Documents\My Games\Hellgate\Logs` to read-only.

**3. Save corruption / character loss.** Multiple reports of characters disappearing after crashes, reboots during play, or OneDrive/Steam Cloud overwriting newer saves with older ones. The Steam build **uses the same save path as the 2007 retail version** (`Documents\My Games\Hellgate\Save`), causing conflicts when both are installed. Community advice: manually back up the Save folder each session; disable OneDrive syncing of that folder.

**4. DX9/DX10.** The Steam build **ships DX9-only, 32-bit only — no DX10 and no 64-bit exe** (both present in the 2007 retail + v1.2 patch), confirmed by PCGamingWiki. This removes the Extreme/DoF/motion-blur shader preset and higher-fidelity rendering. Users repeatedly note the retail+Revival build "got 64 bit support and DX10" and looked far better.

**5. Resolution / widescreen / FOV / ultrawide / high-refresh.**
- Widescreen and 4K work; ultrawide (21:9, 32:9) is **Vert- and stretched** — the game snaps to the nearest 16:9 and adds black bars.
- Fixes: WSGF/Flawless Widescreen plugins and jackfuste's hex-edit fixes; a newer WSGF-Discord "drop-and-play" patch that also corrects **model FOV** (which hex edits/FWS do not); manual `Settings.xml` edits (`nFrameBufferWidth/Height`, refresh numerator).
- **No in-game FOV slider**; first-person FOV "hurts my eyes" with no official option.
- **60 FPS cap** tied to VSync/refresh; changing refresh (e.g., 144 Hz) sometimes won't stick and must be forced via `Settings.xml` (set folder/file read-only so it isn't reset on launch).

**6. Controller support.** No native controller support; workable via Steam Input custom layouts (community configs exist for Steam Deck). Users report a **mouse-drift bug** (mouse creeps to a screen edge) caused by a plugged-in controller sending spurious signals — unplug the controller or clear controller bindings.

**7. Windows 10/11.** "Jumping/creeping mouse" in fullscreen on Win10 Fall Update — fix via **Disable fullscreen optimizations** on `Hellgate.exe`/`hellgate_sp_x86.exe`, or play borderless. Particle-death FPS spikes mitigated by **Windows 7 compatibility mode**. Some users need the disable-fullscreen-optimizations tweak just to launch.

**8. Linux / Proton / Steam Deck.** Playable on Steam Deck (30+ hrs reported) but **controls need heavy tweaking** (mouse+kbd UI bolted onto a controller). DXVK/Vulkan is the recommended path on Linux for the FPS bug. Some Deck players report not hitting the raycast bug through mid-game, but it's not eliminated.

**9. Audio.** Separate sound/music/UI volume sliders and up to 7.1 output exist; the most common "audio" complaint is the game **freezing with the last sound effect looping** during the 1 FPS/crash events rather than a distinct audio bug. A pink/missing-texture bug is also reported occasionally.

### B. UI / Quality-of-life pain points (the user's target area)

- **Inventory/stash size & management.** Only ~2 stash tabs; the "buy more tabs" button is a **disabled leftover paid MMO feature**; **no shared stash** for offline (a top request — critical for twinking/gearing alts). No **auto-sort** button. No easy item comparison beyond the base tooltip. HanbitSoft's own launch patch notes reveal the intended (but weak) expansion model: "Added 2 inventory expansion items: 1. Inventory bag (36 slots) 2. Parts pack (72 slots)… You can store up to 2 Inventory bags and one Parts pack in your inventory." Contra's mod tried to enlarge inventory (12×6→50×6) and stash (10×6→260×6) but **the extra slots render visually yet are bugged and unusable** — per a modder on ti360's Steam guide: "the increased inventory and stash spaces… get restored back to the defaults in terms of accessibility (visually the extra slots are there but they look bugged and can't be used). From 50x6 to 12x6 slots for the inventory, and from 260x6 to 10x6 slots for the stash (1st tab)." This strongly suggests the grid dimensions are validated/hardcoded in the exe, not just data.
- **HUD clutter.** Oversized weapon icons, a rudimentary chat box, and defunct cash-shop buttons block a large fraction of the screen; **years-old requests to scale or hide the HUD have no working solution.** The only documented UI lever is editing chat-window dimensions in `Settings.xml` (don't set 0×0). The chat box reappears on every zone change.
- **Tooltip/clutter overload.** Reviews cite "billions of unnecessary tooltips, screen clutter, cheap UI" inherited from the F2P MMO.
- **Map/navigation.** The **"Field Reset Button" on the minimap doesn't work** (workaround: exit and re-log to reset a map). `/stuck` exists to unstick/teleport.
- **Keybinding.** Remapping is flaky; some Win10 installs lack a `<Control>` section in `Settings.xml`; changing a binding in-game then exiting can force the file to write, after which manual XML edits (with read-only) stick.
- **Loot management.** No auto-pickup/auto-salvage in vanilla; Contra's mod adds an **Auto-Dismantler** via a daily-quest NPC, showing loot-processing QoL is at least partly data-moddable.

### C. Gameplay / design complaints

- **Repetition & pacing.** Randomized tunnels feel samey; the Global/Tokyo rebalance is described as a "Korean grinder" — more enemies, slower pacing, delayed skill access vs. the 2007 original.
- **Itemization / endgame is gutted.** Veterans report no meaningful late game: bosses drop useless loot, no mythics/double-edged items in normal play, monster levels balanced around ~50, "you can receive only level 30 yellow items." Much of this is a *consequence* of the bugged Cube and drop tables (below).
- **Balance / classes.** AoE-spam is optimal because maps are wall-to-wall trash with few elites; Summoner pet counts and some skills differ from Global; some class content is inaccessible.
- **Difficulty.** The Steam build deliberately eased leveling (per HanbitSoft's notes: reaching levels 20–50 "became 3 times faster," magic-find from kills "improved by 2 times," more champion spawns, and **decreased Nightmare/Hell difficulty**) vs. Global — a plus for some, a dilution for veterans.
- **No Hardcore/permadeath mode**, another common request.

### D. Regressions & missing content vs. 2007 retail / Global

- **No multiplayer** (the disabled MP code is still in the build); **no shared stash**; **no DX10/64-bit**; lower-res textures and cutscenes; **bad English localization** (mistranslated dialogue, "Engrish"); Korean/anime character models and hairstyles; removed flavor/story content.
- **Bugged/inaccessible content** (well-documented community list by VVillows): the **Transmogrifying Cube is nonfunctional** (takes 4 slots, no recipes) without a mod — this alone gates pets, Restoratives, Lucky Skill Expanders, Berial re-fights, the Cow Level, and many unique/set items; **Stonehenge and Base Defense bosses (Moloch, Khargoth, Orpehell, Darkan) don't drop their armor/weapons**, so **5-piece and 7-piece set bonuses are unobtainable** (only 3 of 7 set pieces are reachable); **PvP achievements unobtainable**; **Delux De-Modificator removes a random mod instead of the selected one**; masks/costumes exist in files but aren't obtainable; adrenaline-pill drop rate is punishingly low; some quest mobs don't drop required quest items / kills don't register.
- **Tokyo** content was advertised but **disabled at launch** "temporarily" pending stability (per HanbitSoft's announcement) and, per community consensus, was effectively never delivered/finished; the game has been abandoned by HanbitSoft/T3 since ~2018–2019.

## Existing community fixes/mods (so the user avoids duplicating work)

- **Augmentrex** (alexrp, GitHub, archived 2022) — the canonical 1 FPS fix (`patch-long-ray-vm`, `patch-cc-agent`); in-memory injector, not a data mod.
- **DXVK/Vulkan d3d9 swap** — alternative FPS-bug mitigation; also the Linux path.
- **Contra's "Hellgate London 2018 Modification" (v2.1.0.4)** — data-folder mod: integrates Kikina's **Cube hotfix**, **Stonehenge drop-bug fix** (from ti360), larger stacks (matching London 2038), Nano Forge 100% success to +20, Auto-Dismantler + Luck Boxes via daily NPC, instant vendor refresh, maxed affixes; attempted (bugged) inventory/stash enlargement.
- **Kikina's standalone Transmogrifying Cube partial fix** (data.zip into `\data`) — unlocks the cube grid, weapon/armor/mod mixing (unique+), Berial key and Cow-Level amulet recipes; most create-recipes and pet recipes still don't work.
- **ti360's TransCube + Stonehenge drop-bug fix** (Steam guide) — the drop-table fix Contra integrates.
- **Mayhem's Unofficial Hellgate Steam SP Modification V1.00** (RJ_Mayhem, 2023) — layers on top of Contra's (add, don't replace): restores event/subscriber content as masks/consumables, **6 augments** (up from 3), dye kits, Palladium purchasability; a separate optional "cheaty" skill mod (no cooldowns, +200 skill points).
- **HD texture / visual-effects restore packs** and **higher-quality cutscene** replacements (linked from PCGamingWiki) to claw back some of the Global downgrade.
- **Reanimator (Steam fork: ti360gh/Reanimator-steam)** — the data-table editor that makes all the above possible; edits `.dat/.idx` pack archives and cooked excel tables (items, skills, drops, stats, strings). Note: repacking on Steam **breaks strings** ("missing string") and needs a Verify-integrity workaround; MMO-era anti-cheat/anti-tamper remnants remain and make modding "more annoying."
- **WSGF / Flawless Widescreen plugins, jackfuste hex fixes, and a newer WSGF-Discord model-FOV patch** — ultrawide/FOV.
- **London 2038** (private-server MP) and **Revival SP** — separate projects on the *retail* client, not compatible with the Steam build; frequently recommended as the "better" way to play, which is the user's competitive context.

## What's fixable client-side vs. what needs deeper engine/data work

**Readily fixable via data-table mods (Reanimator-steam, `\data`):** drop tables and the Stonehenge/Base Defense/boss drops; Cube recipes (partial — some create/pet recipes resist fixing); item stats/affixes; stack sizes; augment counts; upgrade success; vendor behavior; loot-salvage helpers (Auto-Dismantler); restoring event/subscriber items; skill tuning/balance; adrenaline availability; localization strings (text edits are possible but **repacking on Steam breaks strings**, requiring a Verify-integrity workaround — a real friction point). These are the **lowest-risk, highest-content-value** targets.

**Fixable via executable/in-memory patching (harder, the user's wheelhouse):** the 1 FPS bug (already in progress) — and there's headroom to improve on Augmentrex's blunt raycast-disable by preserving boss/aiming raycasts; frame cap / high-refresh handling; possibly the disabled minimap Field Reset; a more surgical crash fix (Summoner, radar-device zone change); a real FOV slider; controller/mouse-drift handling.

**Likely need deep engine/exe work or may be impractical (the risky items):**
- **Inventory auto-sort button and enlarged inventory/stash grids** — no data mod has succeeded; enlarged grids come back bugged/unusable, and there's **no evidence UI layout is exposed to the data pipeline**; the HUD appears hardcoded in the exe. An auto-sort button would likely require adding UI + reordering logic against a hardcoded, undocumented UI system — the single hardest item on the user's wishlist. A pragmatic middle path: an **in-memory "sort inventory" hotkey** (like Augmentrex commands) that reorders the item array without touching the visible UI, avoiding the need to add a button.
- **HUD scaling / hiding, permanent chat-box/cash-shop-button removal, tooltip declutter** — same hardcoded-UI barrier; only chat-window dimensions are user-config via `Settings.xml`.
- **True shared stash for offline** — community consensus is it's very hard; Reanimator's item-trading attempts risk save corruption; a real offline shared vault has never been delivered.
- **Restoring DX10/64-bit or MP** — not realistically portable to the Steam Global client.
- **Anti-cheat/anti-tamper remnants from the MMO era** remain in the client and make modding "more annoying" — a general friction the user should expect.

## Recommendations

**Stage 1 — Ship what's proven and high-impact (do first):**
1. Keep hardening the **1 FPS fix**, but aim to beat Augmentrex: preserve raycasting for boss AI (Ash/Oculis) and ranged collision so you don't inherit its "shoot/get shot through walls" and "unkillable boss" side effects. Offer a **toggle hotkey** as a fallback (mirroring Augmentrex's shift+F1 pattern).
2. **Bundle/repair the Transmogrifying Cube + Stonehenge/Base Defense drop tables** (build on Kikina/ti360/Contra). This unlocks the most missing content for the least risk and addresses the #2 complaint.
3. **Fix the minimap Field Reset button** and add **Auto-Dismantler/auto-salvage** loot QoL (data-provable via Contra's mod).

**Stage 2 — Quality-of-life the user specifically wants:**
4. For **inventory auto-sort**, prototype an **in-memory array-reorder hotkey** rather than a new UI button — this sidesteps the hardcoded-UI wall. Validate on a throwaway character with save backups, because inventory manipulation is exactly where save corruption has historically occurred.
5. Investigate whether the **enlarged inventory/stash grid** can be made functional by patching the exe-side slot-count validation (data-only attempts failed). If it can't be made stable quickly, defer — it's a known trap.
6. Add a **FOV slider** and robust **high-refresh/frame-cap** handling; fold in the WSGF model-FOV ultrawide fix.

**Stage 3 — Stability and polish:**
7. Ship a **save-safety layer**: auto-backup of the Save folder on launch/exit, and change the save path away from the retail location to end retail/Steam conflicts.
8. Address the **Summoner and radar-device/zone-change crashes** and the gigabyte-log issue (force logs read-only or cap logging).
9. Consider a **localization pass** on the worst strings — but plan around the Steam string-repack/Verify-integrity friction.

**Benchmarks that should change the plan:**
- If in-memory inventory reordering proves unstable or corrupts saves in testing → drop the auto-sort feature or ship it disabled-by-default with loud warnings.
- If exe-side grid-size patching can be made stable → promote "bigger stash + shared stash" from Stage 2 risk item to a headline feature (it's one of the top community requests).
- If your improved raycast fix eliminates the boss-fight/through-wall side effects → make it the default and deprecate the Augmentrex dependency.
- If Steam string repacking can't be made reliable → keep localization out of scope and focus effort on mechanics/QoL.

## Caveats
- **Frequency figures are qualitative**, inferred from the volume and recency of reviews/threads, not a formal count; the Steam rating is "Mixed" (52% of 1,125 reviews positive; Steambase 57/100 over 2,308 reviews).
- Some root-cause claims (corpse-physics/particle triggers for the 1 FPS bug) are **community folk explanations**; the *confirmed* technical cause is the Havok `queryRayOnTree` raycast storm per the Augmentrex author.
- Whether the game engine truly hardcodes all UI layout is **inferred from the consistent absence of any working UI mod plus the settings.xml-only exception**, not from an explicit developer statement — the user, with disc + Steam builds and reverse-engineering ability, is well-positioned to verify this directly.
- Several community download links (hellgateaus, Google Drive) are reported dead or login-gated in 2024–2025; the user may need to source mod files from mirrors.
- The user should test every change against **backed-up saves** given the documented save-corruption history, especially for anything touching inventory/items.
