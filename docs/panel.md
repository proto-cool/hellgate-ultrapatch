# The dev panel

Enable with `HG_PANEL=1` in the launch options or an empty
`bin/hellgate_panel.on`, then press **Shift+\`** in game. That key is
`CMD_CONSOLE_TOGGLE`'s own binding; the console it opened is compiled out of
this build, so the binding is free. A bare \` is the chat box and is left
alone.

The panel is a draggable window, driven by the mouse. Every tab also has a
`ctrl`+key, because a click also reaches the game (a DINPUT8 button cannot be
swallowed from an EndScene hook) and exclusive fullscreen can pin the cursor:
`ctrl+1`–`ctrl+0` switch to the first ten tabs.

With `HG_PANEL_HTTP=1` the same panel is served on `http://127.0.0.1:7777/`
(loopback only).

Single player only. The Memory, Spawn and Physics tabs change a live process
and can damage a save.

## Tabs

| Tab | What it does |
|---|---|
| **Live** | frame and physics counters, the last 6 s as a graph |
| **Player** | name, unit pointer, flags, a watch list |
| **Mem** | hex window over the player unit: mark a baseline, see what changed, poke or watch a dword |
| **Spawn** | replay a recorded spawn 1/10/100 times |
| **Phys** | observe and override Havok's simulation type (continuous or discrete collision) |
| **Cam** | camera mode, the action camera, first person with melee weapons |
| **Model** | the player's model flags |
| **Light** | point lights and the LOOK values |
| **Shadow** | sun shadows, shadow maps, characters' shadows, shadow debugging |
| **Post** | anti-aliasing (SMAA or MSAA) and ambient occlusion |
| **Log** | the last lines of the log, so a button's result is visible in game |

## Graphics (Light, Shadow and Post tabs)

Per-pixel lights (smooth falloff), shadow fill, PCSS, the fine shadow map
per pixel, characters taking shadows, static objects casting, SMAA and
ambient occlusion are on by default; switching one off restores the stock
behaviour for it. The LOOK values start at stock. Everything changes live
except the choice between SMAA and MSAA, which applies at the next start.
The Light and Shadow controls need the replacement effects (`make shaders`);
without them the tab says so. Each setting is one row: its value, then − and +.

**Ctrl+Alt+Shift+P** (panel open or not) saves a comparison pair to
`<game>/screenshots/`: `hg_<date>_<time>_new.png` is the frame as it is,
`..._stock.png` a frame a few milliseconds later with every setting on this
page switched to stock (the settings come back by themselves). With SMAA on
the device has no MSAA, so the stock shot has no anti-aliasing at all.

| Tab | Control | Effect |
|---|---|---|
| Light | **Per pixel, 5 per model** | up to five spell and torch lights per pixel, on the level and on characters |
| Light | **falloff, specular, strength** | linear (stock) or smooth falloff, highlights on or off, strength (100% = the engine's colour) |
| Light | **SURFACES: gloss, highlights, reflections, reflection blur** | less shine: lower gloss broadens highlights (energy-normalised), strengths scale highlights and cube-map reflections, blur softens reflections; **stock surfaces** resets. Defaults 50%, 75%, 60%, 1.5 |
| Light | **TEXTURES: anisotropic, sharpness** | 16x anisotropic filtering and a -0.25 mip bias on the level and characters; 1x and 0 are stock |
| Light | **LOOK: fill, fog start, sun** | ambient and sky fill, where the fog begins, and sun strength; **2007 look / stock look** presets |
| Shadow | **Shadow fill** | outdoors, a shadow removes only the sun's light, so fill and baked light survive; −/+ in 25% steps |
| Shadow | **Soft shadows (PCSS)** | penumbrae that widen with the distance from caster to ground; **sun size** outdoor and indoor sets how soft |
| Shadow | **bias** | depth bias for PCSS. Lower until feet touch their shadow; speckled shadow on open ground means too low |
| Shadow | **min softness** | the softest a contact shadow gets, in shadow-map texels |
| Shadow | **Fine map per pixel** | outdoors, the sharp 80-unit map wherever it reaches and the zone-wide one beyond, per pixel: no seams between pieces of the level; the wide maps are redrawn every 5 s (−/+) |
| Shadow | **static objects cast** | off / props / all: trees, posts and props (or everything static) cast live shadows outdoors |
| Shadow | **near map reach** | width of the near shadow map in world units (stock 27) |
| Shadow | **Self-shadowing, offset** | characters take their own shadows and others'; *offset* against speckle |
| Shadow | **Player casts a shadow** | clears the NOSHADOW bit on the player's model |
| Shadow | **Map view, Dump maps, Trace maps** | see [graphics.md](graphics.md#outdoor-shadows) |
| Post | **SMAA instead of MSAA** | SMAA 1x; the device loses its MSAA and gains a readable depth buffer. From the next start (`bin/hellgate_smaa.off` when off) |
| Post | **SMAA pass (A/B)** | the SMAA pass alone, live, to compare against no anti-aliasing |
| Post | **Ambient occlusion, show it alone, radius, strength** | screen-space AO after the opaque scene; *show* draws the occlusion by itself |

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
