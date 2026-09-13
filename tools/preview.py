#!/usr/bin/env python3
"""把 PNG 截图转成终端可读的 ASCII 灰度图，用于在没有图形界面的情况下"看"屏幕。

用法：python tools/preview.py <图片> [宽度字符数]
"""

import sys

from PIL import Image

RAMP = " .:-=+*#%@"


def main():
    path = sys.argv[1]
    cols = int(sys.argv[2]) if len(sys.argv) > 2 else 120

    image = Image.open(path).convert("L")
    width, height = image.size
    rows = max(1, int(cols * height / width / 2.2))
    small = image.resize((cols, rows))

    pixels = list(small.getdata())
    lo, hi = min(pixels), max(pixels)
    span = max(1, hi - lo)

    print("# %s  %dx%d -> %dx%d  灰度范围 %d..%d" % (path, width, height, cols, rows, lo, hi))
    for row in range(rows):
        line = []
        for col in range(cols):
            value = pixels[row * cols + col]
            line.append(RAMP[(value - lo) * (len(RAMP) - 1) // span])
        print("".join(line))


if __name__ == "__main__":
    main()