#!/usr/bin/env python3
"""从 VMware 的内存镜像 .vmem 里读 MeOS 的诊断块。

虚拟机运行期间，VMware 会把客户机物理内存映射成 build/<uuid>.vmem，
文件偏移就等于客户机物理地址。内核把中间结果写进 0x6200，这里读出来。

用法：python tools/vmdiag.py [vmem 路径]
"""

import glob
import os
import re
import struct
import sys

STAGES = {0: "还没进保护模式", 1: "IDT 装好了", 2: "硬件参数采完了",
          3: "清屏完了", 4: "字画完了", 5: "显存自检也做完了，已停机"}


def find_vmem():
    if len(sys.argv) > 1:
        return sys.argv[1]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    hits = glob.glob(os.path.join(root, "build", "*.vmem"))
    if not hits:
        raise SystemExit("找不到 .vmem，虚拟机是不是没在跑？")
    return hits[0]


def expected_font_bits():
    """数一数内核字模 font.inc 里置位了多少个点，作为文字白像素数的期望值。"""
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(root, "src", "kernel", "font.inc")
    try:
        with open(path, "r", encoding="utf-8") as fp:
            text = fp.read()
    except OSError:
        return None
    body = text.split("font_bitmap:", 1)[-1]
    return sum(bin(int(t, 16)).count("1")
               for t in re.findall(r"0x([0-9A-Fa-f]{2})", body))


def main():
    path = find_vmem()
    print("vmem: %s  (%d 字节)" % (path, os.path.getsize(path)))
    with open(path, "rb") as fp:
        fp.seek(0x6000)
        params = fp.read(0x20)
        fp.seek(0x6200)
        diag = fp.read(0x40)

    fb, pitch, width, height, bpp, magic, pixb = struct.unpack_from("<IHHHBxII", params, 0)
    print("")
    print("参数块 0x6000")
    print("  MAGIC   = 0x%08X %s" % (magic, "(OK)" if magic == 0x534F454D else "(不对!)"))
    print("  FB      = 0x%08X" % fb)
    print("  PITCH   = %d" % pitch)
    print("  W x H   = %d x %d" % (width, height))
    print("  BPP     = %d   PIXBYTES = %d" % (bpp, pixb))

    (stage, cx, cy, sid, fbs, fbo, bpl, sw, sh, sbpp, sen,
     pfb, palt, barpix, textpix, box) = struct.unpack_from("<16I", diag, 0)
    print("")
    print("诊断块 0x6200")
    print("  阶段    = %d  (%s)" % (stage, STAGES.get(stage, "未知")))
    print("  文字位置 = (%d, %d)" % (cx, cy))
    print("  SVGA_REG_ID              = 0x%08X  (%s)"
          % (sid, "vmware svga2" if sid == 0x90000002 else "?"))
    print("  SVGA_REG_FB_START        = 0x%08X" % fbs)
    print("  SVGA_REG_FB_OFFSET       = 0x%08X" % fbo)
    print("  SVGA_REG_BYTES_PER_LINE  = %d" % bpl)
    print("  SVGA_REG_WIDTH x HEIGHT  = %d x %d" % (sw, sh))
    print("  SVGA_REG_BITS_PER_PIXEL  = %d" % sbpp)
    print("  SVGA_REG_ENABLE          = %d" % sen)
    print("  读写回测 @ 参数块FB      = 0x%08X %s"
          % (pfb, "(可写)" if pfb == 0x5A5AA5A5 else "(读不回)"))
    print("  读写回测 @ SVGA FB_START = 0x%08X %s"
          % (palt, "(可写)" if palt == 0x5A5AA5A5 else "(读不回)"))
    print("  顶部横杠读回白像素       = %d  (该字段保留未用)" % barpix)
    expected = expected_font_bits()
    print("  文字包围盒读回白像素     = %d  (期望 %s，= 字模置位数) %s"
          % (textpix, expected if expected is not None else "?",
             "OK" if expected == textpix else "对不上!"))


if __name__ == "__main__":
    main()