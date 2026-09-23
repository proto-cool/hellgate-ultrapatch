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
# style, above the backpack grid (which starts at y 193). OnLClick borrows
# the hidden security button's handler, which src/uiext.c takes over for
# this control's name. The strings are ours, served by src/uiext.c.
SORT_BUTTON = """
			<button name="ultra sort btn">
				<x>262</x>
				<y>140</y>
				<width>110</width>
				<height>44</height>
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
					<width>100</width>
					<height>42</height>
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

# (file, anchor to insert before, text); each anchor must occur exactly once
EDITS = [
    ("inventory_screen", '\t\t\t<!--<button name="inven security btn" sku="korea_client_online">-->', SORT_BUTTON),
]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        for name, anchor, text in EDITS:
            subprocess.run([sys.executable, HGDAT, "extract", tmp, "uix\\xml\\%s.xml" % name],
                           check=True, stdout=subprocess.DEVNULL)
            src = os.path.join(tmp, "data", "uix", "xml", name + ".xml")
            raw = open(src, "rb").read()
            if raw.count(anchor.encode()) != 1:
                sys.exit("%s: anchor found %d times" % (name, raw.count(anchor.encode())))
            raw = raw.replace(anchor.encode(), text.replace("\n", "\r\n" if b"\r\n" in raw else "\n").encode() + anchor.encode())
            dst = os.path.join(out, "data", "uix", "xml", name + ".xml")
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, "wb").write(raw)
            print("%s -> %s (%d bytes)" % (name, dst, len(raw)))


if __name__ == "__main__":
    main()
