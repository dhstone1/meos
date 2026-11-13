#!/usr/bin/env python3
"""从 .vmem 里核对 ATA / ext2 驱动的诊断字段（FS2 起用，随阶段长胖）。

跟 verify_selftest.py 一个思路：内核跑的时候把状态写进 0x6200 的诊断块，
宿主机从 build/*.vmem 里读回来对账。偏移不抄死，从 kernel.asm 的 equ 里
现解析——改了 D_ATA_* / D_EXT2_* 的位置这里自动跟。ext2 的地面真值
（空闲块数、hello.txt 的 inode 号）从 build/hd-ext2.img 里解析，也不抄死。

用法：
  python tools/verify_ext2.py [vmem 路径]   # 虚拟机开着时跑：活断言
  python tools/verify_ext2.py disk          # 关机后跑：读 hd-flat.vmdk 对账落盘结果

FS2 断言（ATA PIO）：
  D_ATA_STAGE >= 3    —— LBA2 真读通了（disk 命令跑过）
  D_ATA_ERR   == 0    —— 无状态错误、无轮询超时
  D_ATA_LBA   无断言  —— 最后读的扇区随命令变，只打印不判
  D_ATA_COUNT >= 1    —— 至少读过 1 个扇区

FS3 断言（ext2 只读）：
  D_EXT2_STAGE >= 5   —— ls/cat/stat 三类都真读通了磁盘
  D_EXT2_ERR   == 0   —— 无 FS_* 错误码残留
  D_EXT2_CMD   >= 3   —— 三个命令都进过驱动

FS4/FS5 断言（ext2 写/删，vmem 侧）：
  D_EXT2_STAGE >= 6   —— 写路径真跑通（cp 落盘）
  D_EXT2_FREE  == 真值 —— 自检流 cp 分配 1 块后又 rm 释放，回到基线（二次
                        开机同样幂等：先截断复用、再删除）

FS5 断言（disk 侧，关机后）：
  /hd/c1.txt 已不存在（目录条目 ino 清零；rec_len 链仍无缝）
  ino 16（c1.txt 用过的号）已清零；块 43 的位图位 bit42 已释放；
  inode 位图 bit15 已释放；超块/GD free 计数回到镜像基线
  /hd/hello.txt 未被误伤
"""

import glob
import io
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, "src", "kernel", "kernel.asm")
DIAG_ADDR = 0x6200


def eq_value(text, name):
    m = re.search(r"^%s\s+equ\s+(0x[0-9A-Fa-f]+|\d+)" % re.escape(name), text, re.M)
    if not m:
        raise SystemExit("kernel.asm 里找不到 %s" % name)
    return int(m.group(1), 0)


def read_diag(path, names):
    with io.open(ASM, "r", encoding="utf-8", errors="replace") as fp:
        text = fp.read()
    out = {}
    with open(path, "rb") as fp:
        for name in names:
            off = eq_value(text, name)
            fp.seek(DIAG_ADDR + off)
            out[name] = struct.unpack("<I", fp.read(4))[0]
    return out


def find_vmem():
    hits = sorted(glob.glob(os.path.join(ROOT, "build", "*.vmem")),
                  key=os.path.getmtime)
    if not hits:
        raise SystemExit("找不到 .vmem，虚拟机是不是没在跑？")
    return hits[-1]


def ext2_truth():
    """从 build/hd-ext2.img（构建时的原始镜像）解析地面真值。"""
    img = os.path.join(ROOT, "build", "hd-ext2.img")
    with open(img, "rb") as fp:
        fp.seek(1024)                    # 超块在块 1（1024B 盘），不在块 0
        sb = fp.read(1024)
        assert sb[0x38:0x3A] == b"\x53\xEF", "镜像超块魔数不对"
        free = struct.unpack("<I", sb[0x0C:0x10])[0]
        free_ino = struct.unpack("<I", sb[0x10:0x14])[0]
        log_bs = struct.unpack("<I", sb[0x18:0x1C])[0]
        bs = 1024 << log_bs
        fp.seek(2 * bs)
        gd = fp.read(bs)
        inotab = struct.unpack("<I", gd[8:12])[0]
        fp.seek(inotab * bs + 128)
        root_i = fp.read(128)
        root_blk = struct.unpack("<I", root_i[0x28:0x2C])[0]
        fp.seek(root_blk * bs)
        data = fp.read(bs)
        hello_ino = None
        off = 0
        while off < len(data):
            ino, rec = struct.unpack("<IH", data[off:off + 6])
            nlen = data[off + 6]
            if ino and nlen and nlen <= rec - 8:
                name = data[off + 8:off + 8 + nlen].decode("latin-1")
                if name == "hello.txt":
                    hello_ino = ino
            if rec < 8 or rec % 4:
                break
            off += rec
        assert hello_ino, "根目录里没找到 hello.txt"
        return free, free_ino, hello_ino


def walk_root(fp, bs, root_blk):
    fp.seek(root_blk * bs)
    data = fp.read(bs)
    out = {}
    off = 0
    while off < len(data):
        ino, rec = struct.unpack("<IH", data[off:off + 6])
        nlen = data[off + 6]
        if rec < 8 or rec % 4:
            break
        if ino and nlen and nlen <= rec - 8:
            name = data[off + 8:off + 8 + nlen].decode("latin-1")
            out[name] = ino
        off += rec
    return out, off


def check_disk():
    """关机后直读 build/hd-flat.vmdk：cp 的落盘结果对账。"""
    flat = os.path.join(ROOT, "build", "hd-flat.vmdk")
    free0, free_ino0, hello_ino = ext2_truth()
    with open(flat, "rb") as fp:
        fp.seek(1024)
        sb = fp.read(1024)
        free = struct.unpack("<I", sb[0x0C:0x10])[0]
        free_ino = struct.unpack("<I", sb[0x10:0x14])[0]
        fp.seek(2048)
        gd = fp.read(1024)
        inotab = struct.unpack("<I", gd[8:12])[0]
        fp.seek(inotab * 1024 + 128)
        root_i = fp.read(128)
        root_blk = struct.unpack("<I", root_i[0x28:0x2C])[0]
        ents, end = walk_root(fp, 1024, root_blk)

        bad = []
        if "c1.txt" in ents:
            bad.append("根目录仍有 c1.txt（rm 没摘掉条目）：%s" % sorted(ents))
        hello = ents.get("hello.txt")
        if not hello:
            bad.append("根目录丢了 hello.txt：%s" % sorted(ents))
        else:
            fp.seek(inotab * 1024 + (hello - 1) * 128)
            hi = fp.read(128)
            hsize = struct.unpack("<I", hi[0x04:0x08])[0]
            if hsize == 0:
                bad.append("hello.txt size=0（被误伤）")
        # c1.txt 用过的 ino=16（lost+found 占 15 后第一个空闲号）/ 数据块 43
        fp.seek(inotab * 1024 + (16 - 1) * 128)
        ino = fp.read(128)
        links = struct.unpack("<H", ino[0x1A:0x1C])[0]
        mode = struct.unpack("<H", ino[0x00:0x02])[0]
        print("ino16 after rm: mode=0x%X links=%d" % (mode, links))
        if links != 0 or mode != 0:
            bad.append("ino 16 未清干净：mode=0x%X links=%d" % (mode, links))
        # 位图：块 bit 42 ↔ 块 43（bit i ↔ 块 i+1）、inode bit 15 ↔ ino 16
        fp.seek(3 * 1024)
        bb = fp.read(1024)
        if (bb[42 // 8] >> (42 % 8)) & 1:
            bad.append("块位图仍置 bit42（块 43 没释放）")
        fp.seek(4 * 1024)
        ib = fp.read(1024)
        if (ib[15 // 8] >> (15 % 8)) & 1:
            bad.append("inode 位图仍置 bit15（ino 16 没释放）")
        # free 计数落盘：rm 后应回到镜像基线
        if free != free0:
            bad.append("超块 free_blocks=%d，期望 %d（rm 后回基线）" % (free, free0))
        if free_ino != free_ino0:
            bad.append("超块 free_inodes=%d，期望 %d（rm 后回基线）"
                       % (free_ino, free_ino0))
        # 目录块无缝覆盖（rec 洞回归检查）
        if end != 1024:
            bad.append("根目录游走停在 %d（期望 1024：有 rec_len 洞！）" % end)

    if bad:
        print("结果：FAIL")
        for b in bad:
            print("  - " + b)
        return 1
    print("结果：PASS（FS5 落盘：c1.txt 删除、块/inode 回收、计数回基线、目录无缝）")
    return 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "disk":
        return check_disk()
    path = sys.argv[1] if len(sys.argv) > 1 else find_vmem()
    d = read_diag(path, ["D_ATA_STAGE", "D_ATA_ERR", "D_ATA_LBA", "D_ATA_COUNT",
                         "D_EXT2_STAGE", "D_EXT2_ERR", "D_EXT2_INO",
                         "D_EXT2_BLK", "D_EXT2_FREE", "D_EXT2_CMD",
                         "D_EXVEC"])
    free, free_ino, hello_ino = ext2_truth()
    print("vmem: %s" % os.path.basename(path))
    print("D_ATA_STAGE=%d D_ATA_ERR=0x%X D_ATA_LBA=%d D_ATA_COUNT=%d"
          % (d["D_ATA_STAGE"], d["D_ATA_ERR"], d["D_ATA_LBA"], d["D_ATA_COUNT"]))
    print("D_EXT2_STAGE=%d D_EXT2_ERR=%d D_EXT2_INO=%d D_EXT2_BLK=%d "
          "D_EXT2_FREE=%d(真值 %d) D_EXT2_CMD=%d D_EXVEC=%d"
          % (d["D_EXT2_STAGE"], d["D_EXT2_ERR"], d["D_EXT2_INO"],
             d["D_EXT2_BLK"], d["D_EXT2_FREE"], free, d["D_EXT2_CMD"],
             d["D_EXVEC"] & 0xFF))

    bad = []
    if d["D_ATA_STAGE"] < 3:
        bad.append("D_ATA_STAGE=%d，没到 3（扇区还没读通，disk 命令跑了吗？）"
                   % d["D_ATA_STAGE"])
    if d["D_ATA_ERR"] != 0:
        bad.append("D_ATA_ERR=0x%X（bit8=超时，bit0-7=状态寄存器）" % d["D_ATA_ERR"])
    if d["D_ATA_COUNT"] < 1:
        bad.append("D_ATA_COUNT=%d，至少 1（disk 命令读过超块扇区）" % d["D_ATA_COUNT"])

    # ---- FS3：ext2 只读验收面 ----
    if d["D_EXT2_STAGE"] < 5:
        bad.append("D_EXT2_STAGE=%d，没到 5（5 = ls/cat/stat 都真读通了磁盘）"
                   % d["D_EXT2_STAGE"])
    if d["D_EXT2_ERR"] != 0:
        bad.append("D_EXT2_ERR=%d，期望 0（FS_* 错误码残留）" % d["D_EXT2_ERR"])
    if d["D_EXT2_CMD"] < 3:
        bad.append("D_EXT2_CMD=%d，至少 3（ls/cat/stat 三个命令都进过驱动）"
                   % d["D_EXT2_CMD"])
    # ---- FS4：ext2 写验收面 ----
    if d["D_EXT2_STAGE"] < 6:
        bad.append("D_EXT2_STAGE=%d，没到 6（6 = cp 写路径落盘完成）"
                   % d["D_EXT2_STAGE"])
    if d["D_EXT2_FREE"] != free:
        bad.append("D_EXT2_FREE=%d，期望 %d（cp 后又 rm，应回到真值）"
                   % (d["D_EXT2_FREE"], free))
    # ---- 全程无 CPU 异常（idt 向量记录器，见技术文档 §5/§12.5）----
    vec = d["D_EXVEC"] & 0xFF
    if vec != 0:
        bad.append("D_EXVEC=0x%02X（跑到 CPU 异常存根里了，见 D_EXVEC 表）" % vec)

    if bad:
        print("结果：FAIL")
        for b in bad:
            print("  - " + b)
        return 1
    print("结果：PASS（FS2：ATA；FS3：只读；FS4：写；FS5：unlink 回收记账）")
    return 0


if __name__ == "__main__":
    sys.exit(main())

