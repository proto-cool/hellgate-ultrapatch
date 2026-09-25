#!/usr/bin/env python3
"""tools/fx/fogtest.c output -> PNG; several outputs stack into one image.

    fogimg.py <out.png> <raw> [raw ...]      (each 640 x 360 luminance floats)
"""
import struct
import sys
from PIL import Image

W, H = 640, 360
raws = sys.argv[2:]
img = Image.new("L", (W, H * len(raws)))
for i, r in enumerate(raws):
    v = struct.unpack("<%df" % (W * H), open(r, "rb").read())
    tile = Image.new("L", (W, H))
    tile.putdata([max(0, min(255, int(x * 255))) for x in v])
    img.paste(tile, (0, H * i))
img.save(sys.argv[1])
