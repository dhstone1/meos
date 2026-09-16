#!/usr/bin/env python3
"""从 VMware 的内存镜像 .vmem 里读 MeOS 的诊断块。

虚拟机运行期间，VMware 会把客户机物理内存映射成 build/<uuid>.vmem，
文件偏移就等于客户机物理地址。内核把中间结果写进 0x6200，这里读出来。

屏幕上什么都不显示时，靠猜是没用的：是没画上去、画到别处去了，还是画上去了
没显示出来？诊断块把「走到哪一步、硬件参数是什么、键盘收到了几个字节」都记下来，
一刀切开这几类故障。

用法：python tools/vmdiag.py [vmem 路径]
"""

import glob
import os
import re
import struct
import sys

STAGES = {0: "还没进保护模式", 1: "IDT 装好了（含键盘 / 定时器两个真门）",
          2: "硬件参数采完了", 3: "控制台尺寸算好、清屏完了", 4: "中文横幅画完了",
          5: "显存自检做完了", 6: "中断已开，命令行长跑中"}
def find_vmem():
    if len(sys.argv) > 1:
        return sys.argv[1]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    hits = glob.glob(os.path.join(root, "build", "*.vmem"))
    if not hits:
        raise SystemExit("\u627e\u4e0d\u5230 .vmem\uff0c\u865a\u62df\u673a\u662f\u4e0d\u662f\u6ca1\u5728\u8dd1\uff1f")
    return hits[0]


def kernel_src(name):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(root, "src", "kernel", name)
    try:
        with open(path, "r", encoding="utf-8") as fp:
            return fp.read()
    except OSError:
        return ""


def expected_font_bits():
    """\u6570\u4e00\u6570 font.inc \u91cc\u7f6e\u4f4d\u4e86\u591a\u5c11\u4e2a\u70b9\uff0c\u4f5c\u4e3a\u6a2a\u5e45\u767d\u50cf\u7d20\u6570\u7684\u671f\u671b\u503c\u3002"""
    body = kernel_src("font.inc").split("font_bitmap:", 1)[-1]
    return sum(bin(int(t, 16)).count("1")
               for t in re.findall(r"0x([0-9A-Fa-f]{2})", body))


def ascii_cell():
    """\u4ece ascii.inc \u91cc\u628a\u5b57\u7b26\u683c\u5b50\u5c3a\u5bf8\u8bfb\u51fa\u6765\uff0c\u7528\u6765\u63a8\u7b97\u7ec8\u7aef\u662f\u51e0\u5217\u51e0\u884c\u3002"""
    text = kernel_src("ascii.inc")
    w = re.search(r"ASCII_CELL_W\s+equ\s+(\d+)", text)
    h = re.search(r"ASCII_CELL_H\s+equ\s+(\d+)", text)
    if not (w and h):
        return None
    return int(w.group(1)), int(h.group(1))


def main():
    path = find_vmem()
    print("vmem: %s  (%d \u5b57\u8282)" % (path, os.path.getsize(path)))
    with open(path, "rb") as fp:
        fp.seek(0x6000)
        params = fp.read(0x20)
        fp.seek(0x6200)
        diag = fp.read(0x100)

    fb, pitch, width, height, bpp, magic, pixb = struct.unpack_from("<IHHHBxII", params, 0)
    print("")
    print("\u53c2\u6570\u5757 0x6000")
    print("  MAGIC   = 0x%08X %s" % (magic, "(OK)" if magic == 0x534F454D else "(\u4e0d\u5bf9!)"))
    print("  FB      = 0x%08X" % fb)
    print("  PITCH   = %d" % pitch)
    print("  W x H   = %d x %d" % (width, height))
    print("  BPP     = %d   PIXBYTES = %d" % (bpp, pixb))

    (stage, cx, cy, sid, fbs, fbo, bpl, sw, sh, sbpp, sen,
     pfb, palt, barpix, textpix, box) = struct.unpack_from("<16I", diag, 0)
    (kb_irq, kb_last, kb_chars, ticks, con_x, con_y, line_len,
     cmd_num, kb_shift) = struct.unpack_from("<9I", diag, 0x40)
    kb_hist = struct.unpack_from("<8I", diag, 0x64)

    print("")
    print("\u8bca\u65ad\u5757 0x6200")
    print("  \u9636\u6bb5    = %d  (%s)" % (stage, STAGES.get(stage, "\u672a\u77e5")))
    print("  \u6a2a\u5e45\u4f4d\u7f6e = (%d, %d)" % (cx, cy))
    print("  SVGA_REG_ID              = 0x%08X  (%s)"
          % (sid, "vmware svga2" if sid == 0x90000002 else "?"))
    print("  SVGA_REG_FB_START        = 0x%08X" % fbs)
    print("  SVGA_REG_FB_OFFSET       = 0x%08X" % fbo)
    print("  SVGA_REG_BYTES_PER_LINE  = %d" % bpl)
    print("  SVGA_REG_WIDTH x HEIGHT  = %d x %d" % (sw, sh))
    print("  SVGA_REG_BITS_PER_PIXEL  = %d" % sbpp)
    print("  SVGA_REG_ENABLE          = %d" % sen)
    print("  \u8bfb\u5199\u56de\u6d4b @ \u53c2\u6570\u5757FB      = 0x%08X %s"
          % (pfb, "(\u53ef\u5199)" if pfb == 0x5A5AA5A5 else "(\u8bfb\u4e0d\u56de)"))
    print("  \u8bfb\u5199\u56de\u6d4b @ SVGA FB_START = 0x%08X %s"
          % (palt, "(\u53ef\u5199)" if palt == 0x5A5AA5A5 else "(\u8bfb\u4e0d\u56de)"))
    print("  \u9876\u90e8\u6a2a\u6760\u8bfb\u56de\u767d\u50cf\u7d20       = %d  (\u8be5\u5b57\u6bb5\u4fdd\u7559\u672a\u7528)" % barpix)
    expected = expected_font_bits()
    print("  \u6a2a\u5e45\u5305\u56f4\u76d2\u8bfb\u56de\u767d\u50cf\u7d20   = %d  (\u671f\u671b %s\uff0c= \u5b57\u6a21\u7f6e\u4f4d\u6570) %s"
          % (textpix, expected if expected is not None else "?",
             "OK" if expected == textpix else "\u5bf9\u4e0d\u4e0a!"))

    print("")
    print("\u65f6\u949f\u4e0e\u952e\u76d8")
    print("  \u5b9a\u65f6\u5668\u8282\u62cd     = %d   %s"
          % (ticks, "(\u5728\u8dd1\uff0cIRQ0 \u6ca1\u95ee\u9898)" if ticks else "(\u4e00\u6b21\u90fd\u6ca1\u54cd\uff01IRQ0 \u6ca1\u901a)"))
    print("  \u952e\u76d8\u4e2d\u65ad\u6b21\u6570   = %d   %s"
          % (kb_irq, "(\u6536\u5230\u6309\u952e\u4e86)" if kb_irq else "(\u8fd8\u6ca1\u6309\u8fc7\u952e)"))
    print("  \u6700\u8fd1\u626b\u63cf\u7801     = 0x%02X" % kb_last)
    print("  扫描码历史     = %s   (最新在最前)" % (
        " ".join("%02X" % v for v in kb_hist) if any(kb_hist) else "（空）"))
    print("  整屏白像素数   = %d   (自检用)" % struct.unpack_from("<I", diag, 0x84)[0])
    print("  \u89e3\u51fa\u5b57\u7b26\u6b21\u6570   = %d" % kb_chars)
    print("  Shift \u662f\u5426\u6309\u4f4f   = %d" % kb_shift)

    cell = ascii_cell()
    if cell:
        cols, rows = width // cell[0], height // cell[1]
        print("")
        print("\u547d\u4ee4\u884c\u72b6\u6001")
        print("  \u7ec8\u7aef\u7f51\u683c       = %d \u5217 x %d \u884c  (\u6bcf\u683c %dx%d)"
              % (cols, rows, cell[0], cell[1]))
        print("  \u5149\u6807\uff08\u5217, \u884c\uff09  = (%d, %d)" % (con_x, con_y))
        print("  \u5f53\u524d\u8f93\u5165\u884c\u957f\u5ea6 = %d" % line_len)
        print("  \u5df2\u6267\u884c\u547d\u4ee4\u6761\u6570 = %d" % cmd_num)


if __name__ == "__main__":
    main()