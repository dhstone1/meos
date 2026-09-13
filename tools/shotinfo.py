#!/usr/bin/env python3
"""分析 VMware 窗口截图：找出客户机画面区域、统计颜色分布、定位亮像素。

用法：python tools/shotinfo.py <图片>
"""

import sys
from collections import Counter

from PIL import Image


def main():
    path = sys.argv[1]
    img = Image.open(path).convert("RGB")
    w, h = img.size
    px = img.load()
    print("图片尺寸: %dx%d" % (w, h))

    # 整图颜色分布
    colors = Counter(px[x, y] for y in range(0, h, 3) for x in range(0, w, 3))
    print("最常见颜色（采样）：")
    for c, n in colors.most_common(8):
        print("   %-18s %6d" % (str(c), n))

    # 找“纯黑”的最大矩形行范围（客户机画面通常是黑的）
    black_rows = []
    for y in range(h):
        dark = sum(1 for x in range(0, w, 4) if sum(px[x, y]) < 30)
        black_rows.append(dark)

    # 打印每行的暗像素占比，粗看版面
    print("每行暗像素占比（每 20 行采一次）：")
    for y in range(0, h, 20):
        print("   y=%4d  %s" % (y, "#" * int(black_rows[y] / max(1, w // 4) * 40)))

    # 全图亮像素（>200）包围盒
    bright = [(x, y) for y in range(h) for x in range(0, w, 2)
              if px[x, y][0] > 200 and px[x, y][1] > 200 and px[x, y][2] > 200]
    if bright:
        xs = [p[0] for p in bright]
        ys = [p[1] for p in bright]
        print("亮像素(>200)数量=%d  包围盒 x:%d..%d  y:%d..%d"
              % (len(bright), min(xs), max(xs), min(ys), max(ys)))
    else:
        print("没有亮像素")


if __name__ == "__main__":
    main()