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

FS4 断言（ext2 写，vmem 侧）：
  D_EXT2_STAGE >= 6   —— 写路径真跑通（cp 落盘）
  D_EXT2_FREE  == 真值-1 —— cp 分配了 1 个数据块（截断复用则不新分配，二次
                        开机幂等：盘上已有 c1.txt，FREE 停在 真值-1 不再降）

FS4 断言（disk 侧，关机后）：
  /hd/c1.txt 存在（ino=16；lost+found 占 15，bit15）
  内容 == src/ramfs/hello.txt（cp 的源）
  块位图 +bit42（=块 43，c1.txt 数据块——位 i 对应块 i+1 的移位约定；
  lost+found 的块 42 = bit 41）、inode 位图 +bit15、
  超块/GD free 计数已落盘
  根目录块条目游走无缝覆盖到 1024（rec_len 洞回归检查）
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
        c1 = ents.get("c1.txt")
        if not c1:
            print("结果：FAIL（根目录没有 c1.txt：%s）" % sorted(ents))
            return 1
        # lost+found 占 15（mkext2 注入的标准配件目录），c1.txt = 16
        # = 下一个非保留空闲 inode（位 bit15；mkext2 占 1..15）
        if c1 != 16:
            bad.append("c1.txt ino=%d，期望 16（lost+found 占 15 后第一个空闲号）" % c1)
        fp.seek(inotab * 1024 + (c1 - 1) * 128)
        ino = fp.read(128)
        size = struct.unpack("<I", ino[0x04:0x08])[0]
        links = struct.unpack("<H", ino[0x1A:0x1C])[0]
        mode = struct.unpack("<H", ino[0x00:0x02])[0]
        dblk = struct.unpack("<I", ino[0x28:0x2C])[0]
        print("c1.txt: ino=%d mode=0x%X links=%d size=%d blk=%d"
              % (c1, mode, links, size, dblk))
        if mode & 0xF000 != 0x8000:
            bad.append("c1.txt mode 非 reg")
        if links != 1:
            bad.append("c1.txt links=%d，期望 1" % links)
        src = open(os.path.join(ROOT, "src", "ramfs", "hello.txt"), "rb").read()
        if size != len(src):
            bad.append("c1.txt size=%d，期望 %d（= MeFS /hello.txt）" % (size, len(src)))
        fp.seek(dblk * 1024)
        data = fp.read(1024)[:size]
        if data != src:
            bad.append("c1.txt 内容 != src/ramfs/hello.txt（前 16B %r）" % data[:16])
        # 位图：块 bit dblk-1（bit i ↔ 块 i+1）、inode bit c1-1
        fp.seek(3 * 1024)
        bb = fp.read(1024)
        if not (bb[(dblk - 1) // 8] >> ((dblk - 1) % 8)) & 1:
            bad.append("块位图没置 bit%d（c1.txt 数据块 %d）" % (dblk - 1, dblk))
        fp.seek(4 * 1024)
        ib = fp.read(1024)
        if not (ib[(c1 - 1) // 8] >> ((c1 - 1) % 8)) & 1:
            bad.append("inode 位图没置 bit%d（c1.txt 的 ino-1）" % (c1 - 1))
        # free 计数落盘
        if free != free0 - 1:
            bad.append("超块 free_blocks=%d，期望 %d（真值-1）" % (free, free0 - 1))
        if free_ino != free_ino0 - 1:
            bad.append("超块 free_inodes=%d，期望 %d" % (free_ino, free_ino0 - 1))
        # 目录块无缝覆盖（rec 洞回归检查）
        if end != 1024:
            bad.append("根目录游走停在 %d（期望 1024：有 rec_len 洞！）" % end)

    if bad:
        print("结果：FAIL")
        for b in bad:
            print("  - " + b)
        return 1
    print("结果：PASS（FS4 落盘：c1.txt 内容/位图/计数/目录无缝 全对）")
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
    if d["D_EXT2_FREE"] != free - 1:
        bad.append("D_EXT2_FREE=%d，期望 %d（真值-1；二次开机截断复用不新分配）"
                   % (d["D_EXT2_FREE"], free - 1))
    # ---- 全程无 CPU 异常（idt 向量记录器，见技术文档 §5/§12.5）----
    vec = d["D_EXVEC"] & 0xFF
    if vec != 0:
        bad.append("D_EXVEC=0x%02X（跑到 CPU 异常存根里了，见 D_EXVEC 表）" % vec)

    if bad:
        print("结果：FAIL")
        for b in bad:
            print("  - " + b)
        return 1
    print("结果：PASS（FS2：ATA；FS3：只读；FS4：写路径 + 分配记账）")
    return 0


if __name__ == "__main__":
    sys.exit(main())

