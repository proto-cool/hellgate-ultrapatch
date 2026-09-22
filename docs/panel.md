# The dev panel

Enable with `HG_PANEL=1` in the launch options or an empty
`bin/hellgate_panel.on`, then press **Shift+\`** in game. That key is
`CMD_CONSOLE_TOGGLE`'s own binding; the console it opened is compiled out of
this build, so the binding is free. A bare \` is the chat box and is left
alone.

The panel is a draggable window, driven by the mouse. Every tab also has a
`ctrl`+key, because a click also reaches the game (a DINPUT8 button cannot be
swallowed from an EndScene hook) and exclusive fullscreen can pin the cursor:
`ctrl+1`–`ctrl+8` switch tabs.

With `HG_PANEL_HTTP=1` the same panel is served on `http://127.0.0.1:7777/`
(loopback only).

Single player only. The Memory, Spawn and Physics tabs change a live process
and can damage a save.

## Tabs

| Tab | What it does |
|---|---|
| **Live** | frame and physics counters, the last 6 s as a graph |
| **Player** | name, unit pointer, flags, a watch list |
| **Memory** | hex window over the player unit: mark a baseline, see what changed, poke or watch a dword |
| **Spawn** | replay a recorded spawn 1/10/100 times |
| **Physics** | observe and override Havok's simulation type (continuous or discrete collision) |
| **Camera** | camera mode, the action camera, first person with melee weapons |
| **Model** | graphics controls, the player's model flags |
| **Log** | the last lines of the log, so a button's result is visible in game |

## Graphics (Model tab)

Per-pixel lights (smooth falloff), shadow fill, PCSS and the fine shadow
map per pixel are on by default; switching one off restores the stock
behaviour for it. The LOOK values start at stock. Everything changes live. The controls
need the replacement effects (`make shaders`); without them the section says
so.

| Control | Effect |
|---|---|
| **Per-pixel lights** | up to five spell and torch lights per pixel, on the level and on characters |
| **strength, falloff, specular** | strength (100% = the engine's colour), linear (stock) or smooth falloff, highlights on or off |
| **Shadow fill** | outdoors, a shadow removes only the sun's light, so fill and baked light survive. On/off jumps to 100%; −/+ in 25% steps |
| **PCSS soft shadows** | penumbrae that widen with the distance from caster to ground |
| **sun size outdoor / indoor** | how soft PCSS shadows get, separately for outdoor and indoor materials |
| **min softness** | the softest a contact shadow gets, in shadow-map texels |
| **bias** | depth bias for PCSS. Lower until feet touch their shadow; speckled shadow on open ground means too low |
| **LOOK: fill, fog start, sun** | ambient and sky fill, where the fog begins, and sun strength |
| **2007 look / stock look** | presets for the three LOOK values |
| **Player casts shadow** | clears the NOSHADOW bit on the player's model |

The status line shows the shadow-map type (PCSS needs type 2, the default)
and "knob writes", which rises each time the shaders receive new values.

## Camera tab

- **Camera mode**: first person, third person or restore, through the
  engine's own `SetCameraMode`.
- **Action camera** (off by default): an over-the-shoulder offset with left
  and right swap, heights near and far, pitch lift, zoom limit, a true orbit,
  the camera's own collision, and a melee impulse (a small push on hits).
- **Melee first-person unlock** (`HG_FP_MELEE=1` or the toggle): the engine
  forces third person whenever a melee weapon is equipped, at two places
  (`CanUseFirstPerson` at `0x004DBFB5`, and a check at skill start at
  `0x0062B31B`). The unlock bypasses both and is undone when switched off.
  Melee weapons have no first-person model, so expect gaps.

## Spawn tab

The panel cannot build a spawn from nothing: the game's spawn call takes
thirteen dwords of context that nobody has mapped. It records the first
spawn the game performs, verbatim, and replays it on demand. One real spawn
anywhere in the zone arms the buttons; until then the tab says so.

- **Immediate** (default) replays from the physics pump, so a button acts
  at once. It creates an entity part-way through a Havok step.
- **Piggyback** replays only when the game spawns something itself: a
  context the game has just shown to be safe, but only as often as it
  spawns.

## Memory tab

The player's unit struct is mostly unmapped. To find an offset, place the
window, press **Mark**, do the thing (take a hit, pick something up), and
every changed byte is highlighted. Click one to select its dword, then
**Watch it**.
