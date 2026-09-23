# Spike: an Ultrapatch page in the game's Options menu (2026-09-23)

Goal: our graphics settings on a tab of the game's own Options dialog,
built like the inventory Sort button (`src/uiext.c`, `tools/ui/mkuix.py`).
Nothing is built yet. Addresses are for sha256 `401e011d…`.

## Options page

### 1. The dialog and its tabs

- **File:** `data\uix\xml\options.xml` (3007 lines). It is in the
  `UIInitLoad` list ("options", logged by the probe), so the XML override
  applies to it as it does to `inventory_screen`.
- **Structure:** `options dialog` (the whole screen, `options_atlas`) holds
  `options dialog main panel` (800 x 784 at x 400, `starttab 0`,
  OnPostActivate `UIOptionsDialogOnPostActivate`). Inside it: the title,
  four tab buttons, one panel per tab, then Accept / Cancel and the
  "restart needed" label.
- **Tabs are pure data.** Each tab button is a `buttonstyle radiobutton`
  with `<tab>n</tab>`; each tab panel has the same `<tab>n</tab>`. No
  handler switches them. Used: video 0, audio 1, controls 3, game 4
  (music 2 is commented out, button and panel). A new tab is a button and
  a panel with `<tab>5</tab>`.
- **Tab buttons** are 72 x 72 at y 62, x 214 / 314 / 414 / 514, frames
  `option_off_* / option_hi_* / option_on_*`. Five fit centred at x 164 /
  264 / 364 / 464 / 564 (the four stock `<x>` values need replacing: a
  replace edit in mkuix, not an insert).
- **Icons:** `options_atlas` has only video, audio, music, controls and
  game icons. The unused music set (`option_*_music`) is the least
  confusing to borrow; a small label over it can say "ULTRA".
- **Handlers** for this dialog are not in the static table at 0xb94750.
  They are registered by name from a stack table at 0x52495e..0x524a14:
  Accept 0x523447, Cancel 0x524026, OnPostActivate 0x523c70,
  OnPostInactivate 0x523f8c, GraphicOptionsDialogOnPostActivate 0x5239be,
  and the rest. Our controls still bind to `UIinventorySecurityOnClk`.

### 2. Controls

All cdecl, caller pops.

- **Find a control:** `FUN_00472a4b(root, "name", 0)` returns the component.
  The dialog root is `*0x00f274ec` (Accept reads everything through it).
- **Checkboxes** (`buttonstyle checkbox`, frames `box_uncheck` / `box_check`):
  the state is bit 1 (0x2) of the button's +0x29c.
  - `FUN_005ac75a(root, "name", on)`: set it.
  - `FUN_005ac713(root, "name")`: read it.
  - **A handler on the checkbox replaces its toggle.** The stock
    `UIWindowedButtonClicked` (0x522a5d, bound on `OnLButtonDown`) calls
    the default checkbox handler `FUN_005ac82d(comp, msg, wp, lp)` first,
    then reads the new state. Ours must do the same: bind `OnLButtonDown`
    to `UIinventorySecurityOnClk`, call 0x5ac82d, then read the state.
- **Label text:** `FUN_005b8171(label, const wchar_t *text, 0)`. Used by
  `UILabelSetToFocusUnitName` (0x5b2654) with a 128-character buffer.
- **Sliders:** `FUN_005b0117(root, "name", float value, 1)` sets one; the
  reader `FUN_005b001c(root, "name")` returns a float in a register
  Ghidra shows as XMM0, so sliders are harder. −/+ steppers with a value
  label are simpler: `slider_button_left` / `slider_button_right` (and
  `... mouse`) are the atlas's arrow buttons.

### 3. Refreshing our controls

- **No hook needed:** bind our tab panel's `<OnPostActivate>` to
  `UIinventorySecurityOnClk` too. The dispatcher sees the name "ultra
  settings panel", fills every checkbox and value label from our current
  values, and returns 1. Anything else keeps going to the stock handler.
- **Alternative:** a MinHook on `UIOptionsDialogOnPostActivate` (0x523c70,
  `(comp, msg, wp, lp)`, returns 1): call it, then fill ours.

### 4. Accept and Cancel

- **Accept** (0x523447) reads stock controls by name only, and filters on
  the message (a key press other than Enter is ignored). Ours are never
  read.
- **Cancel** (0x524026) copies the game's settings snapshot back
  (0xf723c4 → 0xf724a0) and closes the dialog. It never looks at unknown
  controls, so our values are left alone. It also does not revert them.
- **Recommendation:** apply our settings live, as the dev panel does, and
  save them to disk when changed. Cancel then does not undo them, which
  should be said in the tab's footer text. A true Cancel would need
  MinHooks on 0x523447 and 0x524026 to save or restore our snapshot,
  including their message filters.

## Proposed XML

Insert the tab button before `      <panel name="video settings panel">`,
and the panel before `      <!-- game settings panel -->` (both unique).
Rows are generated from a table in mkuix; one checkbox row and one stepper
row shown.

```xml
      <button name="options ultra btn">
        <tab>5</tab>
        <frame>option_off_music</frame>
        <litframe>option_hi_music</litframe>
        <downframe>option_on_music</downframe>
        <x>564</x>
        <y>62</y>
        <width>72</width>
        <height>72</height>
        <buttonstyle>radiobutton</buttonstyle>
        <tooltipstring>ultra options tab</tooltipstring>
        <OnLButtonDownSnd>ButtonUIOptionsMouseTab</OnLButtonDownSnd>
      </button>

      <panel name="ultra settings panel">
        <tab>5</tab>
        <x>66</x>
        <y>182</y>
        <width>668</width>
        <height>740</height>
        <fontsize>22</fontsize>
        <OnPostActivate>UIinventorySecurityOnClk</OnPostActivate>
        <panel name="ultra settings screen">
          <width>668</width>
          <height>740</height>
          <flexborder name="ultra settings flexborder">
            <frameTL>textbox_top_left</frameTL> <frameTM>textbox_top_mid</frameTM>
            <frameTR>textbox_top_right</frameTR> <frameML>textbox_mid_left</frameML>
            <frameMM>textbox_mid_mid</frameMM> <frameMR>textbox_mid_right</frameMR>
            <frameBL>textbox_btm_left</frameBL> <frameBM>textbox_btm_mid</frameBM>
            <frameBR>textbox_btm_right</frameBR>
          </flexborder>

          <!-- checkbox row -->
          <label name="ultra fog label">
            <string>ultra opt fog</string>
            <x>50</x> <y_rel>15</y_rel> <autosize>1</autosize>
            <red>247</red> <green>142</green> <blue>30</blue>
          </label>
          <button name="ultra fog btn">
            <frame>box_uncheck</frame> <downframe>box_check</downframe>
            <x>10</x> <y_rel>-5</y_rel> <width>35</width> <height>35</height>
            <buttonstyle>checkbox</buttonstyle>
            <OnLButtonDown>UIinventorySecurityOnClk</OnLButtonDown>
            <check_on_sound>ButtonAccept</check_on_sound>
            <check_off_sound>ButtonReject</check_off_sound>
          </button>

          <!-- stepper row: name, -, value, + -->
          <label name="ultra shafts label">
            <string>ultra opt shafts</string>
            <x>50</x> <y_rel>45</y_rel> <autosize>1</autosize>
            <red>247</red> <green>142</green> <blue>30</blue>
          </label>
          <button name="ultra shafts dn">
            <frame>slider_button_left</frame> <litframe>slider_button_left mouse</litframe>
            <x>430</x> <y_rel>-3</y_rel> <width>30</width> <height>30</height>
            <OnLClick>UIinventorySecurityOnClk</OnLClick>
          </button>
          <label name="ultra shafts val">
            <x>462</x> <y_rel>0</y_rel> <width>100</width> <height>30</height>
            <align>center</align>
          </label>
          <button name="ultra shafts up">
            <frame>slider_button_right</frame> <litframe>slider_button_right mouse</litframe>
            <x>564</x> <y_rel>0</y_rel> <width>30</width> <height>30</height>
            <OnLClick>UIinventorySecurityOnClk</OnLClick>
          </button>
        </panel>
      </panel>
```

`y_rel` stacks rows as the game tab does. Whether a label's text can be
empty at load and filled later is untested; give it a placeholder string
key if not.

## Risks

1. **Tab icon:** borrowing the music icon is a placeholder.
2. **Checkbox click order:** the toggle-then-read order is taken from
   0x522a5d. Unverified in game for a handler bound through entry 227.
3. **`y_rel` on the stepper's buttons and value label** is relative to the
   previous sibling. Check the row layout in game, or use absolute `y`.
4. **Options before a game is loaded** (the main menu): our settings must
   be readable there, or the tab should be greyed out.
5. **Cancel does not revert our changes** (section 4).
