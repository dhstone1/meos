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

NET_STAGES = {0: "还没开始找网卡", 1: "PCI 扫描完了",
              2: "找到网卡，信息已记下", 3: "e1000 复位完成",
              4: "收发环建好了", 5: "链路已 up",
              6: "ARP 解析到网关", 7: "ICMP 收到应答",
              8: "DNS 解析完成", 9: "网络全部就绪"}
def find_vmem():
    if len(sys.argv) > 1:
        return sys.argv[1]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    hits = glob.glob(os.path.join(root, "build", "*.vmem"))
    if not hits:
        raise SystemExit("找不到 .vmem，虚拟机是不是没在跑？")
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
    """数一数 font.inc 里置位了多少个点，作为横幅白像素数的期望值。"""
    body = kernel_src("font.inc").split("font_bitmap:", 1)[-1]
    return sum(bin(int(t, 16)).count("1")
               for t in re.findall(r"0x([0-9A-Fa-f]{2})", body))


def ascii_cell():
    """从 ascii.inc 里把字符格子尺寸读出来，用来推算终端是几列几行。"""
    text = kernel_src("ascii.inc")
    w = re.search(r"ASCII_CELL_W\s+equ\s+(\d+)", text)
    h = re.search(r"ASCII_CELL_H\s+equ\s+(\d+)", text)
    if not (w and h):
        return None
    return int(w.group(1)), int(h.group(1))


def main():
    path = find_vmem()
    print("vmem: %s  (%d 字节)" % (path, os.path.getsize(path)))
    with open(path, "rb") as fp:
        fp.seek(0x6000)
        params = fp.read(0x20)
        fp.seek(0x6200)
        diag = fp.read(0x200)

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
    (kb_irq, kb_last, kb_chars, ticks, con_x, con_y, line_len,
     cmd_num, kb_shift) = struct.unpack_from("<9I", diag, 0x40)
    kb_hist = struct.unpack_from("<8I", diag, 0x64)

    print("")
    print("诊断块 0x6200")
    print("  阶段    = %d  (%s)" % (stage, STAGES.get(stage, "未知")))
    print("  横幅位置 = (%d, %d)" % (cx, cy))
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
    print("  横幅包围盒读回白像素   = %d  (期望 %s，= 字模置位数) %s"
          % (textpix, expected if expected is not None else "?",
             "OK" if expected == textpix else "对不上!"))

    print("")
    print("时钟与键盘")
    print("  定时器节拍     = %d ms   %s"
          % (ticks, "(在跑，IRQ0 没问题)" if ticks else "(一次都没响！IRQ0 没通)"))
    print("  键盘中断次数   = %d   %s"
          % (kb_irq, "(收到按键了)" if kb_irq else "(还没按过键)"))
    print("  最近扫描码     = 0x%02X" % kb_last)
    print("  扫描码历史     = %s   (最新在最前)" % (
        " ".join("%02X" % v for v in kb_hist) if any(kb_hist) else "（空）"))
    print("  整屏白像素数   = %d   (自检用)" % struct.unpack_from("<I", diag, 0x84)[0])
    print("  解出字符次数   = %d" % kb_chars)
    print("  Shift 是否按住   = %d" % kb_shift)

    cell = ascii_cell()
    if cell:
        cols, rows = width // cell[0], height // cell[1]
        print("")
        print("命令行状态")
        print("  终端网格       = %d 列 x %d 行  (每格 %dx%d)"
              % (cols, rows, cell[0], cell[1]))
        print("  光标（列, 行）  = (%d, %d)" % (con_x, con_y))
        print("  当前输入行长度 = %d" % line_len)
        print("  已执行命令条数 = %d" % cmd_num)
    print("")
    print("网络")
    # 逐个按绝对偏移读：MAC 字段是 6 字节 + 2 字节填充，
    # 按一串 dword 解包会从这里开始整体错位（踩过）。
    def u32(off):
        return struct.unpack_from("<I", diag, off)[0]

    net_stage = u32(0x100)
    vendor = u32(0x104)
    device = u32(0x108)
    busdev = u32(0x10C)
    mmio = u32(0x110)
    irq = u32(0x114)
    link = u32(0x120)
    net_tx = u32(0x124)
    net_rx = u32(0x128)
    arp_rx = u32(0x134)
    icmp_tx = u32(0x138)
    icmp_rx = u32(0x13C)
    dns_ip = u32(0x140)
    dns_ok = u32(0x144)
    lastip = u32(0x148)
    rtt = u32(0x14C)
    localip = u32(0x150)
    neterr = u32(0x154)
    mac = diag[0x118:0x11E]
    gwmac = diag[0x12C:0x132]

    def ip4(v):
        return "%d.%d.%d.%d" % (v & 0xFF, (v >> 8) & 0xFF,
                                (v >> 16) & 0xFF, (v >> 24) & 0xFF)

    def macs(b):
        return ":".join("%02X" % x for x in b)

    print("  进度        = %d  (%s)" % (net_stage, NET_STAGES.get(net_stage, "未知")))
    if net_stage >= 1:
        print("  PCI         = %04X:%04X   位置 dev<<11|fn<<8 = 0x%04X"
              % (vendor, device, busdev))
        print("  BAR0 (MMIO) = 0x%08X   IRQ = %d" % (mmio, irq))
        scan = struct.unpack_from("<8I", diag, 0x158)
        listed = ["%04X:%04X" % (v & 0xFFFF, v >> 16) for v in scan if v]
        print("  总线0 设备  = %s" % (", ".join(listed) if listed else "（一个都没读到！）"))
    if any(mac):
        print("  本机 MAC    = %s" % macs(mac))
    if any(gwmac):
        print("  网关 MAC    = %s" % macs(gwmac))
    print("  CTRL 寄存器 = 0x%08X" % link)
    print("  发/收帧数   = %d / %d" % (net_tx, net_rx))
    pktlen = u32(0x180)
    if pktlen:
        raw = diag[0x184:0x184 + 64]
        print("  最近收帧    = %d 字节" % pktlen)
        for i in range(0, 64, 16):
            print("      " + " ".join("%02X" % b for b in raw[i:i + 16]))
    print("  ARP 应答    = %d" % arp_rx)
    print("  ICMP 发/收  = %d / %d" % (icmp_tx, icmp_rx))
    print("  本机 IP     = %s" % ip4(localip))
    if dns_ok:
        print("  DNS 解析    = %s" % ip4(dns_ip))
    if lastip:
        print("  最近 ping   = %s  往返 %d ms" % (ip4(lastip), rtt))
    if neterr:
        print("  错误码      = %d" % neterr)



if __name__ == "__main__":
    main()