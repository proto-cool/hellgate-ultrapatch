# The dev panel

Enable with `HG_PANEL=1` in the launch options or an empty
`bin/hellgate_panel.on`, then press **Shift+\`** in game. That key is
`CMD_CONSOLE_TOGGLE`'s own binding; the console it opened is compiled out of
this build, so the binding is free. A bare \` is the chat box and is left
alone.

The panel is a draggable window, driven by the mouse: a sidebar of pages on
the left, the page on the right. `ctrl+1`–`ctrl+0` switch to the first ten
pages, because a click also reaches the game (a DINPUT8 button cannot be
swallowed from an EndScene hook) and exclusive fullscreen can pin the cursor.

On the graphics pages every setting is one row, read from the saved settings
by key (`src/settings.c`): a value row has a bar showing where the value sits
in its range (drag or click it) with a tick at the default, the value, and
− / +; a switch row a checkbox; a choice row segmented buttons. A row that
differs from its default has an accent bar at its left and an accent value.
**Reset page (N changed)**, at the page title's right when something is
changed, puts that page's settings back to their defaults. Rows marked "not
saved" are session-only and are not reset.

With `HG_PANEL_HTTP=1` the same panel is served on `http://127.0.0.1:7777/`
(loopback only).

Single player only. The Memory, Spawn and Physics tabs change a live process
and can damage a save.

## Pages

| Section | Page | What it does |
|---|---|---|
| Graphics | **Lighting** | point lights, the LOOK values and presets, surfaces, textures |
| | **Shadows** | sun shadows (fill, PCSS), shadow maps, characters' shadows, point-light shadows |
| | **Image** | anti-aliasing (SMAA or MSAA), sharpening, ambient occlusion, light spill, contact shadows, particles |
| | **HDR** | the float scene, tone map, auto exposure |
| | **Atmosphere** | volumetric fog, bloom, colour grade |
| Gameplay | **Camera** | camera mode, the action camera, first person with melee weapons (not saved yet, so no reset) |
| Debug | **Graphics debug** | shadow-map view, dump and trace, A/B passes (SMAA, AO and fog alone), the HDR scan, counters |
| | **Performance** | frame and physics counters, the last 6 s as a graph |
| | **Player** | name, unit pointer, flags, a watch list |
| | **Memory** | hex window over the player unit: mark a baseline, see what changed, poke or watch a dword |
| | **Spawn** | replay a recorded spawn 1/10/100 times |
| | **Physics** | observe and override Havok's simulation type (continuous or discrete collision) |
| | **View model** | the player's model flags |
| | **Log** | the last lines of the log, so a button's result is visible in game |

## Graphics pages

**Saved.** Every graphics setting a player can change (not the debug
views) is saved to `bin\ultrapatch.ini` a frame after it changes, and
loaded at the next start (`src/settings.c`). Deleting the file restores
the defaults; a line can be edited or removed by hand.

Per-pixel lights (smooth falloff), shadow fill, PCSS, the fine shadow map
per pixel, characters taking shadows, static objects casting, SMAA and
ambient occlusion are on by default; switching one off restores the stock
behaviour for it. The LOOK values default to stock (0). **2007 look** sets them all, **stock look** zeroes them. Everything changes live
except the choice between SMAA and MSAA, which applies at the next start.
The Light and Shadow controls need the replacement effects (`make shaders`);
without them the tab says so. Each setting is one row: its value, then − and +.

**Ctrl+Alt+Shift+P** (panel open or not) saves a comparison pair to
`<game>/screenshots/`: `hg_<date>_<time>_new.png` is the frame as it is,
`..._stock.png` a frame a few milliseconds later with every setting on this
page switched to stock (the settings come back by themselves). With SMAA on
the device has no MSAA, so the stock shot has no anti-aliasing at all.

**Ctrl+Alt+Shift+S** holds the same stock view until pressed again, for A/B
by eye; "STOCK" shows at the top of the screen meanwhile.

| Tab | Control | Effect |
|---|---|---|
| Light | **Per pixel, 5 per model** | up to five spell and torch lights per pixel, on the level and on characters |
| Light | **falloff, specular, strength** | linear (stock) or smooth falloff, highlights on or off, strength (100% = the engine's colour) |
| Light | **SURFACES: gloss, highlights, reflections, reflection blur** | less shine: lower gloss broadens highlights (energy-normalised), strengths scale highlights and cube-map reflections, blur softens reflections; outdoor materials only unless **also indoors** (indoors the two specular lights carry the shape); **stock surfaces** resets. Defaults 50%, 75%, 60%, 1.5 |
| Light | **TEXTURES: anisotropic, sharpness** | 16x anisotropic filtering and a -0.25 mip bias on the level and characters; 1x and 0 are stock |
| Light | **normal-map detail: sun, rest; bicubic light maps** | the level's normal maps in its diffuse light (70% on the direct sun, 50% on the rest; 0 = stock) and smooth light maps instead of stair-stepped ones |
| Light | **LOOK: fill outdoors, fill indoors, fog start, sun** | ambient and sky fill (indoor materials have their own), where the fog begins, and sun strength; **2007 look / stock look** presets |
| Shadow | **Shadow fill** | outdoors, a shadow removes only the sun's light, so fill and baked light survive; indoors, only the light above the ambient and SH floor; −/+ in 25% steps |
| Shadow | **Soft shadows (PCSS)** | penumbrae that widen with the distance from caster to ground; **sun size** outdoor and indoor sets how soft |
| Shadow | **bias** | depth bias for PCSS. Lower until feet touch their shadow; speckled shadow on open ground means too low |
| Shadow | **min softness** | the softest a contact shadow gets, in shadow-map texels |
| Shadow | **Fine map per pixel** | outdoors, the sharp 80-unit map wherever it reaches and the zone-wide one beyond, per pixel: no seams between pieces of the level; the wide maps are redrawn every 5 s (−/+) |
| Shadow | **static objects cast** | off / props / all: trees, posts and props (or everything static) cast live shadows outdoors |
| Shadow | **Stable casters** | casters found by a 3x wider search (buildings are found by their size, not their corner) and walls faded for the camera still cast; off = stock (shadows popped as you walked) |
| Shadow | **near map reach** | width of the near shadow map in world units (stock 27) |
| Shadow | **POINT-LIGHT SHADOWS: on, bias, softness** | the strongest fire, torch or spell light near you casts shadows of characters and props (you included) onto the level and characters; the status shows the light's position and the re-drawn caster count |
| Shadow | **Self-shadowing, offset** | characters take their own shadows and others'; *offset* against speckle |
| Shadow | **Player casts a shadow** | clears the NOSHADOW bit on the player's model |
| Shadow | **Map view, Dump maps, Trace maps** | see [graphics.md](graphics.md#outdoor-shadows) |
| Post | **SMAA instead of MSAA** | SMAA 1x; the device loses its MSAA and gains a readable depth buffer. From the next start (`bin/hellgate_smaa.off` when off) |
| Post | **SMAA pass (A/B)** | the SMAA pass alone, live, to compare against no anti-aliasing |
| Post | **soft particles** | smoke, fire and spell sprites fade where they meet geometry instead of cutting into it; fade distance 0.6 units, 0 = off |
| Post | **lit by nearby lights, darker in sun shadow** | smoke, dust and ash take the colour of fires and lamps near them, and darken in the sun's shadow; 0 = stock |
| Post | **HDR scene, tone map, exposure, shoulder, bloom from, scan** | the 3D scene in a float target, tone-mapped before the UI (switches at the next frame); *tone map* off is the stock clamp (A/B); *scan* logs out-of-range pixels |
| Post | **sharpen (CAS)** | contrast-adaptive sharpening after SMAA, 50% by default; 0 = off |
| Post | **Ambient occlusion, show it alone, radius, strength, less in sun, colour bounce** | screen-space AO after the opaque scene; *show* draws the occlusion by itself; *less in sun* eases it off where the sun lights the surface; *colour bounce* tints and lifts surfaces next to lit coloured ones |
| Post | **Light spill: strength, reach; debug: spill alone, spill light** | indoors, with HDR: the engine's nearby lights (lamps, fires, portals, spells) light the surfaces around them wider and softer, in world space, blocked by what stands between (the depth buffer, or the shadow cube); *reach* is a share of each light's own radius; 0 strength is stock |
| Post | **Contact shadows: strength, reach; debug: contact alone** | indoors: a short march from each surface towards its light through the depth buffer darkens where something close blocks it, so feet and props meet the floor; 0 strength is stock |
| Atmos | **Volumetric fog, show it alone, density outdoors / indoors, distance haze, sun shafts, shafts reach, light halos** | light scattered by the air: sun shafts through the sun's shadow maps outdoors, halos around fires and lamps (the shadowing light casts shafts); *show* draws the scattered light alone |
| Atmos | **Bloom, colour grade, bloom, threshold, saturation, contrast, shadow tint, vignette** | bright light bleeds softly into its surroundings; the grade tints the shadows towards the level's fog colour |

The status line shows the shadow-map type (PCSS needs type 2, the default)
and "knob writes", which rises each time the shaders receive new values.

## Camera page

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

## Spawn page

The panel cannot build a spawn from nothing: the game's spawn call takes
thirteen dwords of context that nobody has mapped. It records the first
spawn the game performs, verbatim, and replays it on demand. One real spawn
anywhere in the zone arms the buttons; until then the tab says so.

- **Immediate** (default) replays from the physics pump, so a button acts
  at once. It creates an entity part-way through a Havok step.
- **Piggyback** replays only when the game spawns something itself: a
  context the game has just shown to be safe, but only as often as it
  spawns.

## Memory page

The player's unit struct is mostly unmapped. To find an offset, place the
window, press **Mark**, do the thing (take a hit, pick something up), and
every changed byte is highlighted. Click one to select its dword, then
**Watch it**.
