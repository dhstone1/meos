#!/usr/bin/env python3
"""从系统 TrueType 字体渲染方块点阵汉字，生成内核用的字模表。

用法：
    python tools/genfont.py [输出文件] [文本] [字号]

默认渲染 32x32 的「Hi，我是meos，很高兴来到这个世界~」，写出 NASM 可 include 的汇编片段，
并在终端把字模并排打印出来，方便肉眼确认字形。

字模格式（每个汉字都一样）：
    FONT_GLYPH_H 行，每行 FONT_ROW_BYTES 字节，行优先、高位在左（MSB = 最左边的像素）。
    每个字合计 FONT_GLYPH_H * FONT_ROW_BYTES 字节。
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont

THRESHOLD = 100

FONT_CANDIDATES = [
    r"C:\Windows\Fonts\simsun.ttc",
    r"C:\Windows\Fonts\simhei.ttf",
    r"C:\Windows\Fonts\msyh.ttc",
]


def pick_font():
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            return path
    raise SystemExit("找不到可用的中文字体")


def render_glyph(ch, font_path, size):
    font = ImageFont.truetype(font_path, size)
    img = Image.new("L", (size, size), 0)
    ImageDraw.Draw(img).text((size // 2, size // 2), ch, fill=255,
                             font=font, anchor="mm")
    return img


def to_rows(img, width, height):
    px = img.load()
    rows = []
    for y in range(height):
        value = 0
        for x in range(width):
            if px[x, y] >= THRESHOLD:
                value |= 1 << (width - 1 - x)
        rows.append(value)
    return rows


def row_text(value, width):
    return "".join("#" if value & (1 << (width - 1 - x)) else "."
                   for x in range(width))


def print_preview(text, glyphs, width, height):
    print("字模预览（每个字 %dx%d）：" % (width, height))
    for y in range(height):
        line = "  ".join(row_text(glyphs[ch][y], width) for ch in text)
        print("  " + line)
    print("  字符顺序：" + " ".join(text))


def write_inc(path, text, glyphs, font_name, width, height):
    row_bytes = width // 8
    out = []
    out.append("; 本文件由 tools/genfont.py 自动生成，请勿手工修改")
    out.append("; 字体：%s   文本：%s   字号：%dx%d" % (font_name, text, width, height))
    out.append("; 每个字 %d 行 x %d 字节 = %d 字节，行优先、高位在左"
               % (height, row_bytes, height * row_bytes))
    out.append("")
    out.append("FONT_GLYPH_W   equ %d" % width)
    out.append("FONT_GLYPH_H   equ %d" % height)
    out.append("FONT_ROW_BYTES equ %d" % row_bytes)
    out.append("FONT_GLYPH_LEN equ %d" % (height * row_bytes))
    out.append("FONT_CHAR_NUM  equ %d" % len(text))
    out.append("")
    out.append("font_bitmap:")
    for ch in text:
        out.append("    ; %s" % ch)
        for value in glyphs[ch]:
            raw = value.to_bytes(row_bytes, "big")
            out.append("    db " + ", ".join("0x%02X" % b for b in raw))
    out.append("")
    with open(path, "w", encoding="utf-8") as fp:
        fp.write("\n".join(out))


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join("src", "kernel", "font.inc")
    text = sys.argv[2] if len(sys.argv) > 2 else "Hi，我是meos，很高兴来到这个世界~"
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 32
    if size % 8:
        raise SystemExit("字号必须是 8 的倍数，当前 %d" % size)

    font_path = pick_font()
    glyphs = {}
    for ch in dict.fromkeys(text):
        glyphs[ch] = to_rows(render_glyph(ch, font_path, size), size, size)

    print_preview(text, glyphs, size, size)
    write_inc(out_path, text, glyphs, os.path.basename(font_path), size, size)
    print("")
    print("已写出 %s（%d 个字，每字 %d 字节，共 %d 字节）"
          % (out_path, len(text), size * size // 8, len(text) * size * size // 8))


if __name__ == "__main__":
    main()