# Spike: an auto-sort button in the inventory (2026-09-23)

Goal: a "Sort" button on the inventory panel that repacks the backpack.
Nothing is built yet. Addresses are for sha256 `401e011d…`.

## Recommendation

- **Button:** add it through the game's own UI XML, loaded in memory by the DLL.
- **Moves:** send the same client move messages a drag and drop sends.
- **No direct writes:** no server calls, no inventory writes. Every move goes through the server's own checks.
- **Worst case:** a half-sorted bag, never a broken one.

## Evidence

### 1. Item moves

- **No sort routine exists.** No sort, arrange or auto-place strings are tied to inventory. The only sort strings are for email, party, character select and render lists.
- **The command table.** It is registered in `FUN_0095c5e8`, with entries 0x24 bytes apart at 0xf6f…:
  - +0x00: the handler.
  - +0x1c: the name.
  - +0x20: the id word (written by `FUN_005a52bf`).
- **Inventory commands in that table:**

  | Name | Id | Handler | Payload |
  |---|---|---|---|
  | `sCCmdInvEquip` | 0x2c | 0x51a9e1 | |
  | `sCCmdInvDrop` | 0x2d | 0x51abca | |
  | `sCCmdInvPut` | 0x2e | 0x51ac56 | byte loc at +0xc |
  | `sCCmdInvEquipcheck` | 0x2f | 0x51ad54 | |
  | `sCCmdInvMove` | 0x30 | 0x51adb8 | +0xc loc, +0xd x, +0xe y |
  | `sCCmdInvSwap` | 0x31 | 0x51aedb | |
  | `sCCmdItemInvPut` | 0x32 | 0x51b1a9 | |

- **What InvMove does:** it checks the item may go in that location (`FUN_0062f729`), then calls `FUN_00630a60`. That lands in the core location change, `FUN_0062f9b2(__FILE__, __LINE__, loc, x, y, flags)`, which has 27 callers.
  - `FUN_0062f93b` tests that the target cells are free.
  - Flag bit 0 skips that test. Do not use it.
- **Server replies:** on failure the server calls `FUN_00630945`, which resends the item's location to the client (id 0x2e), so the UI snaps back. Location changes reach the client as `sSCmdChangeInvLocation`.
- **Client side:** every message goes out through `FUN_0055e258`, with the id in ECX and a message pointer on the stack (218 callers).
  - The UI's "put item at location x, y" helper is `FUN_006309fa`, with 27 callers, including `UIEquipCursorItem` and the inventory UI near `UIInventoryShowBackpack`.
  - Registers: EDI = container, ESI = item. Stack: loc, x, y.
  - The message it builds: `{u16 0xffff; +4 container id; +8 item id; +0xc loc; +0xd x; +0xe y}`. That is exactly InvMove's payload.
  - It sends id **0x2d**, not 0x30. The table and the wire ids need a runtime probe before anything relies on a number.
- **Unit ids:** a unit's id is at unit+0x2dc. `FUN_004b98dc(game=EAX, id)` looks a unit up by id, using the hash at game+0x184.
- **Pickup:** `PickupAddToInventory` (the string at 0xa468ec) is a pickup script action, not a grid search. The free-cell test above is what auto-placement needs.

### 2. The inventory model

- **Grids:**
  - Grid locations come from the `INVLOC_DATA` table.
  - `FUN_00628332(loc)` returns a location's record. +0x78 is its type, and +0x38 is a flag `FUN_00630213` tests before auto-placement.
  - The grid's width and height fields are not mapped yet.
- **Item sizes:** from the items table's `invwidth` / `invheight` columns (strings at 0xa1d80c / 0xa1d818).
- **Walking items:** `FUN_0062b6e6` returns an item's container, and `FUN_0062b6d3` walks up to the outermost owner. The iterator over a location's items is not located. `UIInvGridOnPaint` (string at 0xa4877c) draws each grid's items, so it must use it, and it is the next thing to decompile.
- **Backpack locations:** the backpack grid is `bigpack` (the "big pack" invgrid in `inventory_screen.xml`). The optional packs are `backpack`, `backpack_medium`, `backpack_small` and `extend_backpack1-3`.

### 3. Where the sort runs

- **Thread:** UI handlers run on the main thread, like the UI's own drag and drop, so the sort runs inside the button's click handler.
- **Planning:** plan the whole sort against a copy of the grid, then send one move per item, in an order that only ever targets free cells. The server handles them in order, and each one is checked on its own.
- **Server-side alternative:** calling the server directly (`FUN_00630a60` with register arguments on the server's thread) would skip the client queue. It needs the server's game pointer and thread, and it bypasses the path the UI tests every day. Not recommended.

### 4. The button

- **Panels are plain XML:** `data\uix\xml\inventory_screen.xml` in `hellgate000.dat`, 233 KB, not cooked.
  - The loader builds the path as `data\uix\xml\<name>.xml` (`FUN_00473c45`).
  - It reads the file from the archive, and the load-complete callback `FUN_0047381d` gets a request block with the buffer at +0x38.
  - The component builder `FUN_004871fc` copies each control's `name` to component+0 (0x80 bytes).
- **Handlers bind by name:** `<OnLClick>Name</OnLClick>` resolves through a static table of `{name, fn}` pairs at 0xb94750–0xb94ee0 (242 entries).
  - Handlers are cdecl `(component, msg, wparam, lparam)` and return 1 when handled. `UIInventoryOnCloseButtonClicked` is at 0x48da70.
  - New names cannot be added without editing that table.
- **Can reuse:** the inventory panel already holds a hidden button, `inven security btn`, with `visible 0`, bound to `UIinventorySecurityOnClk`. That handler (0x48d3cf, 9 bytes) opens the MMO-era security dialog.
- **Data route (recommended):**
  1. When `inventory_screen.xml` loads, the DLL swaps in a copy from `<game>\override\data\uix\xml\` that adds `<button name="ultra sort btn">`. It reuses a stock atlas frame and binds `OnLClick` to `UIinventorySecurityOnClk`.
  2. A MinHook on 0x48d3cf checks component+0. For "ultra sort btn" it sorts; anything else goes to the stock handler.
  - The button then looks, lights and clicks like a game button, and moves with the panel.
  - Tooltip: `tooltipstring` takes a string key, and adding strings means repacking the string tables. Reuse an existing key (`email sort label` exists; check its text) or leave the tooltip off.
- **DLL-drawn route (fallback):** draw the button with `src/ui.c` over the panel's rect, read from its component.
  - It needs the component's position fields and its visibility.
  - Its clicks also reach the game underneath, so they'd have to be filtered like the Alt latch.
  - It would look foreign. Only worth it if the XML swap fails.
- **Data round trip:** writing cooked data is untested (`hguncook.py` only reads). The journal (graphics step 0) recorded that the engine opens only `serverlist.xml` from disk, so loose files are not read. Repacking the `.dat` breaks Steam's integrity check, and the community says it breaks strings.
  - The in-memory swap avoids all of that. It is also the general answer to "how do we ship data edits".

## Plan

1. **Probe (half a day).**
   - Hook `FUN_0055e258` and log the id and first 16 bytes of each message while dragging items. That settles the 0x2d / 0x30 question.
   - Hook `FUN_0047381d` and log the request block, to confirm the buffer (+0x38), its size field and the path.
2. **XML override (1 day).** A generic in-memory swap: any archive UI file with a copy under `override\` is replaced before parsing. Log each swap. With no override file, stock is unchanged.
3. **Inventory access (1 day).** Decompile `UIInvGridOnPaint` to get the item iterator and the grid size. Read each item's size and position. Show the backpack as text in the dev panel to check it.
4. **Sort (1–2 days).**
   - Plan: sort by type, then size (largest first), then name, and pack first fit, column-major, into a simulated grid.
   - Moves: send one move per item, in an order where every target is free in the simulation. If a cycle blocks, move a blocker to any free cell first. If there is no room, stop.
   - Tests: the planner is plain C, so it goes in `test/ui.c`, with full-bag and cycle cases.
5. **Button (half a day).** The XML control, the hook on 0x48d3cf, and a Sort row on the panel as a backup trigger.
6. **In game (1 day).**
   - Full bag, stacks, quest items, bags that are open or closed, the cursor holding an item, and a merchant open.
   - Save, quit and reload to check the new positions persist.

Effort: 4–6 days.

## Unknowns, riskiest first

1. **Wire ids and batching:** the id the client really sends for a move (the table says 0x30, the sender says 0x2d), and whether dozens of moves sent in one frame all arrive in order.
2. **Item iteration and grid size:** the iterator and the grid's width and height, both at unmapped offsets.
3. **The load callback:** the exact buffer and size fields in `FUN_0047381d`'s request block, and whether the swap needs its own buffer allocation.

## Probe results (2026-09-23, `src/invprobe.c`)

**Moves.** A drag and drop sends two messages, both id **0x2d** (the
command table's 0x30 is not the wire id): pick up to location 0x36 (the
cursor), then put at location 0x18 (the backpack grid) with x at +0xd and y
at +0xe. Container id at +4 is 0 (the player), item id at +8. Four drags
(a redeemer, a grappler, two swords):

    ff ff 00 00 00 00 00 00 1d 00 00 00 36 00 00 00   pick up item 0x1d
    ff ff 00 00 00 00 00 00 1d 00 00 00 18 04 08 06   put at 0x18 (4, 8)
    ... 0x0a -> (4, 10), 0x0d -> (3, 9), 0x0c -> (3, 6)

The byte at +0xf is not zeroed by the sender. All sends are on the main
thread (the same tid as the UI loads). Open: whether a put without the pick
up (grid to grid) is accepted; the sort can fall back to pairs.

**UI loads.** `UIInitLoad(name)` runs for 32 files, twice (startup and
after character select); `inventory_screen` is one. The load callback's
request record: +0x4 the XML buffer, +0xc / +0x18 / +0x38 its size
(inventory_screen.xml: 0x38e9a, 233 KB), +0x28 the context, whose +0x0 is
the path (`data\uix\xml\inventory_screen.xml`) and +0x140 the same buffer.
All on the main thread. Override plan: in the callback, for a path with a
file under `override\`, point those fields at our buffer for the call and
put the game's back afterwards, so the game frees its own allocation.
