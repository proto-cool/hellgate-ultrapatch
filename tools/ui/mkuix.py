#!/usr/bin/env python3
"""
Build our UI XML overrides from the game's own files (src/uiext.c loads
them in memory in place of the archive's copy).

    mkuix.py <outdir>

Extracts the stock files with tools/data/hgdat.py, applies the edits below,
and writes <outdir>/data/uix/xml/<name>.xml. The game's XML is not kept in
this repository; only the edits are.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
HGDAT = os.path.join(HERE, "..", "data", "hgdat.py")

# The inventory's Sort button: the character sheet's tab frames and label
# style, in the empty header space under the Palladium bar, right-aligned
# with the grid (the bottom strip is covered by the HUD in game). OnLClick borrows
# the hidden security button's handler, which src/uiext.c takes over for
# this control's name. The strings are ours, served by src/uiext.c.
SORT_BUTTON = """
			<button name="ultra sort btn">
				<x>305</x>
				<y>142</y>
				<width>80</width>
				<height>40</height>
				<frame>stats_tab_norm</frame>
				<litframe>stats_tab_high</litframe>
				<downframe>stats_tab_select</downframe>
				<stretch>1</stretch>
				<independentactivate>1</independentactivate>
				<visible>1</visible>
				<OnLButtonDownSnd>ButtonMerchantPanelTabs</OnLButtonDownSnd>
				<OnLClick>UIinventorySecurityOnClk</OnLClick>
				<tooltipstring>ultra sort tooltip</tooltipstring>
				<label>
					<x>5</x>
					<y>0</y>
					<width>70</width>
					<height>38</height>
					<string>ultra sort</string>
					<align>center</align>
					<autosizefont>1</autosizefont>
					<red>0</red>
					<green>0</green>
					<blue>0</blue>
					<dropshadowred>180</dropshadowred>
					<dropshadowgreen>180</dropshadowgreen>
					<dropshadowblue>180</dropshadowblue>
				</label>
			</button>
"""

# The Ultrapatch tab in Options (src/optpage.c), in the Video tab's style: a
# fifth tab button (the game's own wrench "options icon" from main_atlas, 64
# px, centred in the 72 px tab slot: options_atlas has no spare tab icon),
# then section bars (the Video tab's open windowshade frames), the Look
# dropdown (its shadows combo), checkboxes in two columns, -/+ steppers in
# two columns, and its defaults button. Row names must match g_checks and
# g_steps in src/optpage.c; the labels are our strings "ultra opt <row>".
OPT_CHECKS = ["hdr", "ao", "fog", "bloom", "grade", "pcss", "lights", "plshadow"]
OPT_STEPS = ["shafts", "density", "aostr", "bounce", "bloomi", "sharpen", "vignette"]
ROW = 40                                    # pixels between rows
COL_X = (10, 340)                           # the two columns
Y_LOOK = 36
Y_EFFECTS = Y_LOOK + 48
Y_CHECKS = Y_EFFECTS + 40
Y_TUNING = Y_CHECKS + ROW * ((len(OPT_CHECKS) + 1) // 2) + 4
Y_STEPS = Y_TUNING + 40
Y_DEFAULTS = Y_STEPS + ROW * ((len(OPT_STEPS) + 1) // 2) + 12
FOOTER_Y = Y_DEFAULTS + 52

OPT_TAB = """
      <button name="options ultra btn">
        <tab>5</tab>
        <texture>main_atlas</texture>
        <frame>options icon</frame>
        <litframe>options icon lit</litframe>
        <downframe>options icon hi</downframe>
        <x>568</x>
        <y>66</y>
        <width>64</width>
        <height>64</height>
        <buttonstyle>radiobutton</buttonstyle>
        <tooltipstring>ultra opt tab</tooltipstring>
        <OnLButtonDownSnd>ButtonUIOptionsMouseTab</OnLButtonDownSnd>
      </button>
"""


def orange_label(name, key, x, y, click=False):
    return """          <label name="%s">
            <string>%s</string>
            <x>%d</x>
            <y>%d</y>
            <autosize>1</autosize>
            <red>247</red>
            <green>142</green>
            <blue>30</blue>
%s          </label>
""" % (name, key, x, y, "            <OnLButtonDown>UIClickSiblingButton</OnLButtonDown>\n" if click else "")


def section(name, key, y):
    """A section bar: the Video tab's open windowshade header, not clickable."""
    return """          <button name="ultra %s bar">
            <framemid>Windowshade_open_mid</framemid>
            <frameleft>Windowshade_open_left</frameleft>
            <frameright>Windowshade_open_right</frameright>
            <x>9</x>
            <y>%d</y>
            <width>650</width>
            <height>32</height>
            <label name="ultra %s bar label">
              <x>10</x>
              <y>2</y>
              <autosize>1</autosize>
              <string>%s</string>
            </label>
          </button>
""" % (name, y, name, key)


LOOK_COMBO = """          <combobox name="ultra look combo">
            <x>300</x>
            <y>%d</y>
            <width>300</width>
            <height>28</height>
            <labelx>8</labelx>
            <labely>3</labely>
            <labelwidth>300</labelwidth>
            <labelheight>20</labelheight>
            <animtime>200</animtime>
            <font>Eurostile</font>
            <rendersection>DialogMasks</rendersection>
            <buttonupframemid>dropmenu_top_mid</buttonupframemid>
            <buttonupframeleft>dropmenu_top_left</buttonupframeleft>
            <buttonupframeright>dropmenu_top_right_open</buttonupframeright>
            <buttondownframemid>dropmenu_top_mid</buttondownframemid>
            <buttondownframeleft>dropmenu_top_left</buttondownframeleft>
            <buttondownframeright>dropmenu_top_right_closed</buttondownframeright>
            <dropdownheight>120</dropdownheight>
            <bordersize>8</bordersize>
            <itemred>40</itemred>
            <itemgreen>150</itemgreen>
            <itemblue>208</itemblue>
            <highlightred>255</highlightred>
            <highlightgreen>255</highlightgreen>
            <highlightblue>255</highlightblue>
            <highlightbkred>64</highlightbkred>
            <highlightbkgreen>64</highlightbkgreen>
            <highlightbkblue>64</highlightbkblue>
            <autosize>1</autosize>
            <listflexborder>1</listflexborder>
            <frameML>dropmenu_mid_left</frameML>
            <frameMM>dropmenu_mid_mid</frameMM>
            <frameMR>dropmenu_mid_right</frameMR>
            <frameBL>dropmenu_btm_left</frameBL>
            <frameBM>dropmenu_btm_mid</frameBM>
            <frameBR>dropmenu_btm_right</frameBR>
            <highlightframe>dropmenu_hilite</highlightframe>
            <tooltipstring>ultra opt look tip</tooltipstring>
          </combobox>
"""

DEFAULTS_BTN = """          <button name="ultra defaults btn">
            <texture>inventory_atlas</texture>
            <frame>trade accept button</frame>
            <downframe>trade accept button lit</downframe>
            <litframe>trade accept button mouse</litframe>
            <x>209</x>
            <y>%d</y>
            <width>250</width>
            <height>42</height>
            <OnLClick>UIinventorySecurityOnClk</OnLClick>
            <label name="ultra defaults label">
              <width>250</width>
              <height>42</height>
              <fontsize>24</fontsize>
              <string>ultra opt defaults</string>
              <align>center</align>
              <red>0</red>
              <green>0</green>
              <blue>0</blue>
              <dropshadowred>255</dropshadowred>
              <dropshadowgreen>255</dropshadowgreen>
              <dropshadowblue>255</dropshadowblue>
            </label>
          </button>
"""


def opt_panel():
    rows = [section("look", "ultra opt sec look", 0),
            orange_label("ultra look name", "ultra opt look", 20, Y_LOOK + 3),
            LOOK_COMBO % Y_LOOK,
            section("effects", "ultra opt sec effects", Y_EFFECTS)]
    for i, r in enumerate(OPT_CHECKS):
        x, y = COL_X[i % 2], Y_CHECKS + ROW * (i // 2)
        rows.append(orange_label("ultra %s label" % r, "ultra opt " + r, x + 40, y + 5, True))
        rows.append("""          <button name="ultra %s btn">
            <frame>box_uncheck</frame>
            <downframe>box_check</downframe>
            <x>%d</x>
            <y>%d</y>
            <width>35</width>
            <height>35</height>
            <buttonstyle>checkbox</buttonstyle>
            <OnLButtonDown>UIinventorySecurityOnClk</OnLButtonDown>
            <check_on_sound>ButtonAccept</check_on_sound>
            <check_off_sound>ButtonReject</check_off_sound>
          </button>
""" % (r, x, y))
    rows.append(section("tuning", "ultra opt sec tuning", Y_TUNING))
    for i, r in enumerate(OPT_STEPS):
        x, y = COL_X[i % 2], Y_STEPS + ROW * (i // 2)
        rows.append(orange_label("ultra %s name" % r, "ultra opt " + r, x + 10, y + 5))
        for side, dx in (("dn", 170), ("up", 284)):
            frame = "slider_button_left" if side == "dn" else "slider_button_right"
            rows.append("""          <button name="ultra %s %s">
            <frame>%s</frame>
            <litframe>%s mouse</litframe>
            <x>%d</x>
            <y>%d</y>
            <width>30</width>
            <height>30</height>
            <OnLClick>UIinventorySecurityOnClk</OnLClick>
          </button>
""" % (r, side, frame, frame, x + dx, y + 3))
        rows.append("""          <label name="ultra %s val">
            <string>ultra opt val</string>
            <x>%d</x>
            <y>%d</y>
            <width>80</width>
            <height>30</height>
            <align>center</align>
          </label>
""" % (r, x + 202, y + 3))
    rows.append(DEFAULTS_BTN % Y_DEFAULTS)
    return """      <panel name="ultra settings panel">
        <tab>5</tab>
        <x>66</x>
        <y>182</y>
        <width>668</width>
        <height>800</height>
        <clipchildren>1</clipchildren>
        <fontsize>22</fontsize>
        <OnPostActivate>UIinventorySecurityOnClk</OnPostActivate>
        <panel name="ultra settings screen">
          <x>0</x>
          <y>0</y>
          <width>668</width>
          <height>%d</height>
          <fontsize>22</fontsize>
          <flexborder name="ultra settings flexborder">
            <frameTL>textbox_top_left</frameTL>
            <frameTM>textbox_top_mid</frameTM>
            <frameTR>textbox_top_right</frameTR>
            <frameML>textbox_mid_left</frameML>
            <frameMM>textbox_mid_mid</frameMM>
            <frameMR>textbox_mid_right</frameMR>
            <frameBL>textbox_btm_left</frameBL>
            <frameBM>textbox_btm_mid</frameBM>
            <frameBR>textbox_btm_right</frameBR>
          </flexborder>
%s          <label name="ultra footer">
            <string>ultra opt footer</string>
            <x>10</x>
            <y>%d</y>
            <width>648</width>
            <height>30</height>
            <fontsize>18</fontsize>
            <align>center</align>
            <red>170</red>
            <green>170</green>
            <blue>170</blue>
          </label>
        </panel>
      </panel>

""" % (FOOTER_Y + 40, "".join(rows), FOOTER_Y)


# (file, anchor to insert before, text); each anchor must occur exactly once
EDITS = [
    ("inventory_screen", '\t\t\t<!--<button name="inven security btn" sku="korea_client_online">-->', SORT_BUTTON),
    ("options", '      <panel name="video settings panel">', OPT_TAB),
    ("options", '      <!-- game settings panel -->', opt_panel()),
]

# (file, the element that owns it, old, new): the first `old` after the
# element's opening tag. Five tab buttons centred instead of four.
REPLACES = [
    ("options", '<button name="options video btn">', "<x>214</x>", "<x>164</x>"),
    ("options", '<button name="options audio btn">', "<x>314</x>", "<x>264</x>"),
    ("options", '<button name="options controls btn">', "<x>414</x>", "<x>364</x>"),
    ("options", '<button name="options game btn">', "<x>514</x>", "<x>464</x>"),
]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    files = {}
    with tempfile.TemporaryDirectory() as tmp:
        for name in sorted({e[0] for e in EDITS} | {r[0] for r in REPLACES}):
            subprocess.run([sys.executable, HGDAT, "extract", tmp, "uix\\xml\\%s.xml" % name],
                           check=True, stdout=subprocess.DEVNULL)
            files[name] = open(os.path.join(tmp, "data", "uix", "xml", name + ".xml"), "rb").read()
    for name, owner, old, new in REPLACES:
        raw = files[name]
        if raw.count(owner.encode()) != 1:
            sys.exit("%s: %s found %d times" % (name, owner, raw.count(owner.encode())))
        at = raw.index(owner.encode())
        hit = raw.find(old.encode(), at)
        if hit < 0:
            sys.exit("%s: %s not found after %s" % (name, old, owner))
        files[name] = raw[:hit] + new.encode() + raw[hit + len(old):]
    for name, anchor, text in EDITS:
        raw = files[name]
        if raw.count(anchor.encode()) != 1:
            sys.exit("%s: anchor found %d times" % (name, raw.count(anchor.encode())))
        eol = "\r\n" if b"\r\n" in raw else "\n"
        files[name] = raw.replace(anchor.encode(), text.replace("\n", eol).encode() + anchor.encode())
    for name, raw in files.items():
        dst = os.path.join(out, "data", "uix", "xml", name + ".xml")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, "wb").write(raw)
        print("%s -> %s (%d bytes)" % (name, dst, len(raw)))


if __name__ == "__main__":
    main()
