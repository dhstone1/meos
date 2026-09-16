#!/usr/bin/env python3
"""从系统 TrueType 字体渲染点阵字模，生成内核要用的两张表。

用法：
    python tools/genfont.py [横幅输出] [横幅文字] [字号] [ASCII 输出]

两份产物：
  · font.inc   横幅用的中文点阵，默认 32x32，只包含横幅里那几个字；
  · ascii.inc  命令行用的 ASCII 点阵，固定 16x24，覆盖 0x20..0x7E 共 95 个字符。

字模格式：H 行，每行 ROW_BYTES 字节，行优先、高位在左（MSB = 最左边的像素）。
两张表都是定长格子，ASCII 里取第 N 个字就是 ascii_font + (ch - ASCII_FIRST) * ASCII_GLYPH_LEN。
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont

THRESHOLD = 110

FONT_CANDIDATES = [
    r"C:\Windows\Fonts\simsun.ttc",
    r"C:\Windows\Fonts\simhei.ttf",
    r"C:\Windows\Fonts\msyh.ttc",
]

# 命令行字模：16x24 的格子配 20 号 Consolas。
# 试过 8x16（经典 VGA 文字格）和 16x16，前者汉字形被挤成一团、后者的 g 下缘会被切掉；
# 16x24 是能同时装下大写字母和下伸部（g y p q j）的最小格子。
ASCII_CANDIDATES = [
    r"C:\Windows\Fonts\consola.ttf",
    r"C:\Windows\Fonts\lucon.ttf",
    r"C:\Windows\Fonts\cour.ttf",
]
ASCII_FIRST = 0x20
ASCII_LAST = 0x7E
ASCII_W = 16
ASCII_H = 24
ASCII_SIZE = 20
ASCII_BASELINE = 18
ASCII_X0 = 2

ASCII_SAMPLES = [
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "abcdefghijklmnopqrstuvwxyz",
    "0123456789",
    "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~",
    "meos> hello, world!",
]


def pick_font(candidates, what):
    for path in candidates:
        if os.path.exists(path):
            return path
    raise SystemExit("找不到可用的%s字体" % what)


def render_glyph(ch, font_path, size):
    font = ImageFont.truetype(font_path, size)
    img = Image.new("L", (size, size), 0)
    ImageDraw.Draw(img).text((size // 2, size // 2), ch, fill=255,
                             font=font, anchor="mm")
    return img


def render_ascii_glyph(ch, font_path, width, height, size, baseline, x0):
    font = ImageFont.truetype(font_path, size)
    img = Image.new("L", (width, height), 0)
    ImageDraw.Draw(img).text((x0, baseline), ch, fill=255,
                             font=font, anchor="ls")
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
# ---------------------------------------------------------------- 横幅字模

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


# ---------------------------------------------------------------- ASCII 字模

def print_ascii_preview(glyphs):
    print("")
    print("ASCII 字模预览（每格 %dx%d）：" % (ASCII_W, ASCII_H))
    for sample in ASCII_SAMPLES:
        print("  --- %s" % sample)
        for y in range(ASCII_H):
            print("    " + "|".join(row_text(glyphs[ord(c)][y], ASCII_W)
                                    for c in sample))


def check_ascii(glyphs):
    """挑出被格子切掉的字：贴着上/下/左边就说明画不下，得回去调字号或基线。"""
    bad = []
    for code, rows in sorted(glyphs.items()):
        if code == ASCII_FIRST:
            continue
        left = 1 << (ASCII_W - 1)
        right = 1
        if any(v & left for v in rows) or any(v & right for v in rows):
            bad.append((code, "左右被切"))
        elif rows[0] or rows[ASCII_H - 1]:
            bad.append((code, "上下被切"))
        elif not any(rows):
            bad.append((code, "整格是空的"))
    if bad:
        for code, why in bad:
            print("  [警告] 0x%02X %r %s" % (code, chr(code), why))
    else:
        print("  字形检查：%d 个字符全部落在格子里，没有切边" % (len(glyphs) - 1))


def write_ascii_inc(path, font_name, glyphs):
    row_bytes = ASCII_W // 8
    out = []
    out.append("; 本文件由 tools/genfont.py 自动生成，请勿手工修改")
    out.append("; 命令行 ASCII 字模：字体 %s，字号 %d，格子 %dx%d"
               % (font_name, ASCII_SIZE, ASCII_W, ASCII_H))
    out.append("; 覆盖 0x%02X..0x%02X 共 %d 个字符，每字 %d 行 x %d 字节 = %d 字节"
               % (ASCII_FIRST, ASCII_LAST, len(glyphs), ASCII_H, row_bytes,
                  ASCII_H * row_bytes))
    out.append("")
    out.append("ASCII_CELL_W    equ %d" % ASCII_W)
    out.append("ASCII_CELL_H    equ %d" % ASCII_H)
    out.append("ASCII_ROW_BYTES equ %d" % row_bytes)
    out.append("ASCII_GLYPH_LEN equ %d" % (ASCII_H * row_bytes))
    out.append("ASCII_FIRST     equ 0x%02X" % ASCII_FIRST)
    out.append("ASCII_LAST      equ 0x%02X" % ASCII_LAST)
    out.append("ASCII_COUNT     equ %d" % len(glyphs))
    out.append("")
    out.append("ascii_font:")
    for code in range(ASCII_FIRST, ASCII_LAST + 1):
        ch = chr(code)
        out.append("    ; 0x%02X '%s'" % (code, ch))
        for value in glyphs[code]:
            raw = value.to_bytes(row_bytes, "big")
            out.append("    db " + ", ".join("0x%02X" % b for b in raw))
    out.append("")
    with open(path, "w", encoding="utf-8") as fp:
        fp.write("\n".join(out))


def make_ascii(path):
    font_path = pick_font(ASCII_CANDIDATES, "等宽 ASCII")
    glyphs = {}
    for code in range(ASCII_FIRST, ASCII_LAST + 1):
        img = render_ascii_glyph(chr(code), font_path, ASCII_W, ASCII_H,
                                 ASCII_SIZE, ASCII_BASELINE, ASCII_X0)
        glyphs[code] = to_rows(img, ASCII_W, ASCII_H)

    print_ascii_preview(glyphs)
    print("")
    check_ascii(glyphs)
    write_ascii_inc(path, os.path.basename(font_path), glyphs)
    row_bytes = ASCII_W // 8
    print("已写出 %s（%d 个字符，每字 %d 字节，共 %d 字节）"
          % (path, len(glyphs), ASCII_H * row_bytes,
             len(glyphs) * ASCII_H * row_bytes))


# ---------------------------------------------------------------- 入口

def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join("src", "kernel", "font.inc")
    text = sys.argv[2] if len(sys.argv) > 2 else "Hi，我是meos，很高兴来到这个世界~"
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 32
    ascii_path = (sys.argv[4] if len(sys.argv) > 4
                  else os.path.join(os.path.dirname(out_path), "ascii.inc"))
    if size % 8:
        raise SystemExit("字号必须是 8 的倍数，当前 %d" % size)

    font_path = pick_font(FONT_CANDIDATES, "中文")
    glyphs = {}
    for ch in dict.fromkeys(text):
        glyphs[ch] = to_rows(render_glyph(ch, font_path, size), size, size)

    print_preview(text, glyphs, size, size)
    write_inc(out_path, text, glyphs, os.path.basename(font_path), size, size)
    print("")
    print("已写出 %s（%d 个字，每字 %d 字节，共 %d 字节）"
          % (out_path, len(text), size * size // 8, len(text) * size * size // 8))

    make_ascii(ascii_path)


if __name__ == "__main__":
    main()