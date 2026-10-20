#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MeOS · ext2 镜像独立核验器（Linux 文件系统移植 FS1 的"裁判"）

刻意**不 import mkext2 的任何函数**（互盲才有意义）：从裸字节里按 ext2
rev0 的结构自己走一遍超块 → 组描述符 → 位图 → inode 表 → 目录树，
然后对着 src/ext2root/（如果给了 --tree）比内容、比 free 计数。

用法：
    python tools/ext2check.py build/hd-ext2.img
    python tools/ext2check.py build/hd-flat.vmdk --tree src/ext2root
    python tools/ext2check.py vm/hd-flat.vmdk        # FS4 关机后核验写回

退出码：0 = 全绿；1 = 有 FAIL。每条检查独立打印 PASS/FAIL。
"""

import os
import struct
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

MAGIC = 0xEF53
S_IFMT, S_IFDIR, S_IFREG = 0xF000, 0x4000, 0x8000

fails = []
n_pass = 0


def check(name, cond, detail=""):
    global n_pass
    if cond:
        n_pass += 1
        print("  PASS  %s" % name)
    else:
        fails.append(name)
        print("  FAIL  %s%s" % (name, ("：" + detail) if detail else ""))


class Ext2:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.img = f.read()

    # ---- 结构访问 ----
    def u32(self, off):
        return struct.unpack_from("<I", self.img, off)[0]

    def u16(self, off):
        return struct.unpack_from("<H", self.img, off)[0]

    def parse_super(self):
        sb = 1024                                   # bs=1024：超块固定在字节 1024
        self.inodes_count = self.u32(sb + 0x00)
        self.blocks_count = self.u32(sb + 0x04)
        self.free_blocks = self.u32(sb + 0x0C)
        self.free_inodes = self.u32(sb + 0x10)
        self.first_data_block = self.u32(sb + 0x14)
        self.log_bs = self.u32(sb + 0x18)
        self.blocks_per_group = self.u32(sb + 0x20)
        self.inodes_per_group = self.u32(sb + 0x28)
        self.magic = self.u16(sb + 0x38)
        self.state = self.u16(sb + 0x3A)
        self.rev = self.u32(sb + 0x4C)
        self.incompat = self.u32(sb + 0x60)
        self.bs = 1024 << self.log_bs

    def parse_gd(self):
        gd = (self.first_data_block + 1) * self.bs
        self.bbm_block = self.u32(gd + 0x00)
        self.ibm_block = self.u32(gd + 0x04)
        self.itab_block = self.u32(gd + 0x08)
        self.gd_free_blocks = self.u16(gd + 0x0C)
        self.gd_free_inodes = self.u16(gd + 0x0E)
        self.gd_used_dirs = self.u16(gd + 0x10)

    def inode_off(self, ino):
        # v1 约束：单组（inodes_per_group 覆盖全盘 inode）
        return self.itab_block * self.bs + (ino - 1) * 128

    def inode(self, ino):
        off = self.inode_off(ino)
        mode = self.u16(off + 0x00)
        size = self.u32(off + 0x04)     # i_size_lo（rev0 真偏移；0x08 是 atime）
        links = self.u16(off + 0x1A)    # i_links_count（真偏移；0x02 是 uid）
        nblk512 = self.u32(off + 0x1C)
        blocks = [self.u32(off + 0x28 + 4 * i) for i in range(15)]
        return mode, size, links, nblk512, blocks

    def block_used(self, b):
        # 标准（first_data_block=1）：bit i ↔ 块 i+1；块 0（引导区）不占位图
        off = self.bbm_block * self.bs + (b - 1) // 8
        return (self.img[off] >> ((b - 1) % 8)) & 1 == 1

    def inode_used(self, i):
        # 标准：inode N ↔ 位图 bit N-1
        off = self.ibm_block * self.bs + (i - 1) // 8
        return (self.img[off] >> ((i - 1) % 8)) & 1 == 1


def run(path, tree_dir):
    print("[ext2check] %s（%d B）" % (path, os.path.getsize(path)))
    e = Ext2(path)
    e.parse_super()
    e.parse_gd()

    # ---- 超块基本面 ----
    check("魔数 0xEF53", e.magic == MAGIC, "读到 0x%X" % e.magic)
    check("rev 0（v1 驱动只认 rev0）", e.rev == 0, "rev=%d" % e.rev)
    check("块大小 1024（log=%d）" % e.log_bs, e.log_bs == 0)
    check("incompat 特性位为 0", e.incompat == 0, "0x%X" % e.incompat)
    check("s_state = 1（clean）", e.state == 1, "state=%d" % e.state)
    check("盘长 == blocks_count × bs",
          len(e.img) == e.blocks_count * e.bs,
          "%d != %d" % (len(e.img), e.blocks_count * e.bs))
    check("单组：blocks_per_group >= blocks_count",
          e.blocks_per_group >= e.blocks_count - e.first_data_block)
    check("单组：inodes_per_group == inodes_count",
          e.inodes_per_group == e.inodes_count)
    check("inode 表在盘内",
          (e.itab_block + (e.inodes_count * 128 + e.bs - 1) // e.bs) * e.bs
          <= len(e.img))

    # ---- 走树（从根 inode 2）----
    # 收集：路径 -> (mode, size, links, nblk512, blocks)
    seen = {}
    dup = []
    dirs = []

    def walk(ino, rel):
        mode, size, links, nblk512, blocks = e.inode(ino)
        if (mode & S_IFMT) != S_IFDIR:
            print("  FAIL  inode %d 不是目录却出现在目录链里" % ino)
            fails.append("dir-mode")
            return
        seen[rel] = (mode, size, links, nblk512, blocks)
        dirs.append(rel)
        nblocks = 0
        for i, b in enumerate(blocks[:12]):
            if b == 0:
                break
            nblocks += 1
            walk_block(b, ino, rel)
        for b in blocks[12:]:
            if b != 0:
                check("目录 %s 只用直接块（v1 驱动约束）" % (rel or "/"),
                      False, "发现间接指针")
        check("目录 %s：size == 块数×1024" % (rel or "/"),
              size == nblocks * 1024, "size=%d blocks=%d" % (size, nblocks))

    def walk_block(blk, dir_ino, rel):
        off = blk * e.bs
        pos = 0
        while pos < e.bs:
            e_ino, rec, nlen, ftype = struct.unpack_from("<IHBB", e.img, off + pos)
            if rec < 8 or rec % 4 != 0 or pos + rec > e.bs or 8 + nlen > rec:
                check("目录块 %d 的 rec_len 链（off=%d）" % (blk, pos),
                      False, "rec=%d nlen=%d" % (rec, nlen))
                return
            name = e.img[off + pos + 8:off + pos + 8 + nlen].decode("ascii", "replace")
            if e_ino != 0 and name not in (".", ".."):
                if e_ino < 1 or e_ino > e.inodes_count:
                    check("目录项 inode 范围（%s）" % name, False, "ino=%d" % e_ino)
                elif e_ino in seen_inodes:
                    dup.append((rel, name, e_ino))
                else:
                    seen_inodes[e_ino] = (rel + "/" + name) if rel else name
                    mode, size, links, nblk512, blocks = e.inode(e_ino)
                    child = (rel + "/" + name) if rel else name
                    if (mode & S_IFMT) == S_IFDIR:
                        walk(e_ino, child)
                    elif (mode & S_IFMT) == S_IFREG:
                        # 只走直接块（v1 约束），比内容
                        data = bytearray()
                        nb = 0
                        bad_ptr = False
                        for i, b in enumerate(blocks):
                            if b == 0:
                                break
                            if i >= 12:
                                bad_ptr = True
                                break
                            if b >= e.blocks_count:
                                bad_ptr = True
                                break
                            data += e.img[b * e.bs:(b + 1) * e.bs]
                            nb += 1
                        check("文件 %s 块指针合法（且只用直接块）" % child,
                              not bad_ptr)
                        data = bytes(data[:size])
                        files[child] = data
                        inode_meta[e_ino] = (size, nb, links)
                    else:
                        check("文件 %s 的类型（mode 0x%X）" % (child, mode),
                              False, "既不是文件也不是目录")
            pos += rec

    files = {}
    inode_meta = {}
    seen_inodes = {}

    walk(2, "")
    check("inode 不被两个路径共用", not dup, str(dup[:4]))

    # ---- 位图对账：占用 == 可达 ∪ 保留 ----
    # bit i ↔ 块 i+1（块 0 不占位图）；bit blocks_count-1 是位图尾 padding
    # （e2fsck 1.47.3 约定，不算占用），扫描范围 1..blocks_count-1。
    bm_set = set()
    for b in range(1, e.blocks_count - 1):
        if e.block_used(b):
            bm_set.add(b)
    reach_blocks = set()
    for rel, (mode, size, links, nblk512, blocks) in seen.items():
        for b in blocks[:12]:
            if b:
                reach_blocks.add(b)
    for ino, (size, nb, links) in inode_meta.items():
        _, _, _, _, blocks = e.inode(ino)
        for b in blocks[:12]:
            if b:
                reach_blocks.add(b)
    # 保留元数据块：超块/GD/位图/inode 表（块 1 起——块 0 引导区不占位图）
    meta_blocks = set(range(1, e.itab_block +
                            (e.inodes_count * 128 + e.bs - 1) // e.bs))
    check("块位图 == 可达 ∪ 元数据（%d vs %d）"
          % (len(bm_set), len(reach_blocks | meta_blocks)),
          bm_set == reach_blocks | meta_blocks,
          "差集 %s" % sorted(bm_set ^ (reach_blocks | meta_blocks))[:8])

    im_set = set()
    for i in range(1, e.inodes_count + 1):
        if e.inode_used(i):
            im_set.add(i)
    reserved = set(range(1, 11))
    check("inode 位图 == 可达 ∪ 1..10 保留",
          im_set == reserved | set(seen_inodes.keys()) | {2},
          "差集 %s" % sorted(im_set ^ (reserved | set(seen_inodes) | {2}))[:8])

    # free 计数（e2fsck 口径：块位图 padding 位算占用，见 mkext2 注释）
    check("超块 free_blocks == blocks_count - 位图置位数 - padding(1)",
          e.free_blocks == e.blocks_count - len(bm_set) - 1,
          "sb=%d 实际=%d" % (e.free_blocks, e.blocks_count - len(bm_set) - 1))
    check("超块 free_inodes == inodes_count - 位图置位数",
          e.free_inodes == e.inodes_count - len(im_set),
          "sb=%d 实际=%d" % (e.free_inodes, e.inodes_count - len(im_set)))
    check("GD free_blocks 与超块一致",
          e.gd_free_blocks == e.free_blocks)
    check("GD free_inodes 与超块一致",
          e.gd_free_inodes == e.free_inodes)
    check("GD used_dirs == 实际目录数（%d）" % len(dirs),
          e.gd_used_dirs == len(dirs), "gd=%d" % e.gd_used_dirs)

    # ---- i_blocks（512B 扇区）与 links ----
    for rel, (mode, size, links, nblk512, blocks) in seen.items():
        nb = sum(1 for b in blocks[:12] if b)
        check("目录 %s i_blocks == 块数×2" % (rel or "/"), nblk512 == nb * 2,
              "i_blocks=%d nb=%d" % (nblk512, nb))
    for ino, (size, nb, links) in inode_meta.items():
        check("文件 inode %d i_blocks == 块数×2" % ino,
              e.inode(ino)[3] == nb * 2)
        check("文件 inode %d links == 1" % ino, links == 1)

    # ---- 与源树比对 ----
    if tree_dir:
        want = {}
        base = tree_dir
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames.sort()
            for name in sorted(filenames):
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, base).replace("\\", "/")
                with open(full, "rb") as f:
                    want[rel] = f.read()
        check("文件集合与 %s 一致" % base,
              set(files) == set(want),
              "镜像独有 %s / 树独有 %s"
              % (sorted(set(files) - set(want))[:4],
                 sorted(set(want) - set(files))[:4]))
        for rel in sorted(set(files) & set(want)):
            check("内容一致：%s" % rel, files[rel] == want[rel],
                  "%d vs %d 字节" % (len(files[rel]), len(want[rel])))

    print()
    if fails:
        print("[ext2check] %d 项 PASS，%d 项 FAIL：%s"
              % (n_pass, len(fails), fails[:8]))
        sys.exit(1)
    print("[ext2check] %d 项全 PASS" % n_pass)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    path = sys.argv[1]
    tree = None
    if "--tree" in sys.argv:
        tree = sys.argv[sys.argv.index("--tree") + 1]
        if not os.path.isabs(tree):
            tree = os.path.join(ROOT, tree)
    if not os.path.isfile(path):
        # 容错：build/ 里的缺省名
        cand = os.path.join(ROOT, "build", "hd-ext2.img")
        if os.path.isfile(cand):
            path = cand
        else:
            print("[ext2check] 找不到 %s" % path)
            sys.exit(2)
    if tree is None and os.path.isdir(os.path.join(ROOT, "src", "ext2root")):
        tree = os.path.join(ROOT, "src", "ext2root")
    run(path, tree)


if __name__ == "__main__":
    main()
