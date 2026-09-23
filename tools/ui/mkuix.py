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

# The Ultrapatch tab in Options (src/optpage.c): a fifth tab button (the
# game's own wrench "options icon" from main_atlas, 64 px, centred in the
# 72 px tab slot: options_atlas has no spare tab icon), and a panel styled
# like the game tab's: checkboxes on
# the left, -/+ steppers on the right. Row names must match g_rows in
# src/optpage.c; the labels are our strings "ultra opt <row>".
OPT_CHECKS = ["ao", "fog", "bloom", "grade", "pcss", "lights", "smaa", "plshadow", "hdr"]
OPT_STEPS = ["shafts", "density", "bounce", "aostr", "bloomi", "sharpen", "indoor", "vignette"]
ROW = 40                                    # pixels between rows
FOOTER_Y = 16 + ROW * max(len(OPT_CHECKS), len(OPT_STEPS)) + 8

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


def orange_label(name, key, x, y):
    return """          <label name="%s">
            <string>%s</string>
            <x>%d</x>
            <y>%d</y>
            <autosize>1</autosize>
            <red>247</red>
            <green>142</green>
            <blue>30</blue>
%s          </label>
""" % (name, key, x, y, "            <OnLButtonDown>UIClickSiblingButton</OnLButtonDown>\n" if name.endswith("label") and x == 50 else "")


def opt_panel():
    rows = []
    for i, r in enumerate(OPT_CHECKS):
        y = 16 + ROW * i
        rows.append(orange_label("ultra %s label" % r, "ultra opt " + r, 50, y + 5))
        rows.append("""          <button name="ultra %s btn">
            <frame>box_uncheck</frame>
            <downframe>box_check</downframe>
            <x>10</x>
            <y>%d</y>
            <width>35</width>
            <height>35</height>
            <buttonstyle>checkbox</buttonstyle>
            <OnLButtonDown>UIinventorySecurityOnClk</OnLButtonDown>
            <check_on_sound>ButtonAccept</check_on_sound>
            <check_off_sound>ButtonReject</check_off_sound>
          </button>
""" % (r, y))
    for i, r in enumerate(OPT_STEPS):
        y = 16 + ROW * i
        rows.append(orange_label("ultra %s name" % r, "ultra opt " + r, 340, y + 5))
        for side, x in (("dn", 500), ("up", 614)):
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
""" % (r, side, frame, frame, x, y + 3))
        rows.append("""          <label name="ultra %s val">
            <string>ultra opt val</string>
            <x>532</x>
            <y>%d</y>
            <width>80</width>
            <height>30</height>
            <align>center</align>
          </label>
""" % (r, y + 3))
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
