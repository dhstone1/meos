#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MeOS · ext2 虚拟硬盘生成器（Linux 文件系统移植 FS1）

把 src/ext2root/ 种子树打成一个 8 MB 的 ext2 rev0 小盘，再包成 VMware
两文件 flat VMDK（hd.vmdk 描述符 + hd-flat.vmdk 数据），供 FS2 起的
ATA PIO + ext2 驱动使用。

盘面规格（与 docs/Linux文件系统移植方案.md §5.2 对齐；**驱动不依赖**这个
布局——它只信超块和组描述符，这里固定布局只是生成器自己的方便）：

    8 MB = 8192 块 × 1024 B，单组，256 inode，rev 0
    块 0      引导块占位（bs=1024 时 first_data_block=1 的传统，标已用）
    块 1      超块（字节偏移 1024 = 扇区 2，魔数在扇区 2 偏移 0x38）
    块 2      组描述符表（单组 = 1 项 32 B）
    块 3      块位图（8192 位 = 恰好 1 块）
    块 4      inode 位图（256 位 = 32 B，块内其余清零）
    块 5..36  inode 表（256 × 128 B = 32 块）
    块 37     根目录数据（inode 2 的 i_block[0]）
    块 38+    子目录 / 文件数据

inode 编号约定（e2fsprogs 惯例）：1..10 保留（2 = 根），实文件从 11 起。
目录条目按 e2fsprogs 的排法：`.` rec_len=12、`..` rec_len=12、
普通项 8+name_len 上取整到 4、末条目 rec_len 吃满到块尾。
file_type 一律 0（DT_UNKNOWN）：rev0 没有 FILETYPE 特性，
文件/目录判定走 inode 的 i_mode——内核驱动同样这么兜底。

内容纪律与 src/ramfs 相同：只允许可打印 ASCII 0x20..0x7E + 换行
（终端字模只覆盖这些；此函数抄自 mkramfs.py 的 check_text）。

自检（学 mkramfs 的"三面对账"，不过就 SystemExit，构建中断）：
    1. 独立重放：用只读解析器把生成的镜像整棵走回去，跟源树比内容；
    2. 位图逐位对账：置位集合 == 实际占用的块集合；
    3. 目录块结构：rec_len 全部 4 对齐、>=8、块内无缝隙不越界；
    4. 超块 / 组描述符的 free 计数 == 8192-已用 / 256-已分配。

用法：
    python tools/mkext2.py                     # 从 src/ext2root/ 生成三件套
    python tools/mkext2.py --from-mke2fs FILE  # 现成镜像只包 VMDK（互操作验收）
"""

import os
import struct
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SRC_TREE = os.path.join(ROOT, "src", "ext2root")
BUILD_DIR = os.path.join(ROOT, "build")

# ---- 盘面常数 ---------------------------------------------------------------
TOTAL_BLOCKS = 8192          # 8 MB / 1024 B
BLOCK_SIZE = 1024
TOTAL_INODES = 256
INODE_SIZE = 128

SB_BLOCK = 1                 # 超块所在块（bs=1024 传统）
GDT_BLOCK = 2
BBM_BLOCK = 3                # 块位图
IBM_BLOCK = 4                # inode 位图
ITAB_BLOCK0 = 5              # inode 表首块
ITAB_BLOCKS = 32             # 256 × 128 B / 1024 B
ROOT_BLOCK = 37              # 根目录数据块
FIRST_DATA = 38              # 其余数据从这里开始分

ROOT_INO = 2
FIRST_INO = 11               # 1..10 保留，实文件从 11 起
DIRECT_MAX = 12              # 驱动 v1 只走直接块：单文件 <= 12 KB

EXT2_MAGIC = 0xEF53
ROOT_MODE = 0x41ED           # 目录 0755
FILE_MODE = 0x81A4           # 普通文件 0644

# ---- 内容纪律（抄 mkramfs.py 的 check_text） --------------------------------


def check_text(rel, data):
    """文件内容只允许可打印 ASCII + 换行，原因见模块 docstring。"""
    for i, b in enumerate(data):
        ok = b == 0x0A or 0x20 <= b <= 0x7E
        if not ok:
            raise SystemExit(
                "[mkext2] %s 第 %d 字节 0x%02X 不可打印（终端字模没有它）"
                % (rel, i + 1, b))


# ---- 源树收集 ---------------------------------------------------------------


def collect_tree():
    """走 src/ext2root，返回 (entries, contents)：
    entries   = [(rel, is_dir), ...] 深度优先、层内按名字排序，'' 代表根；
    contents  = {rel: bytes} 仅文件。
    """
    entries = []
    contents = {}

    def walk(rel):
        full = os.path.join(SRC_TREE, rel) if rel else SRC_TREE
        for name in sorted(os.listdir(full)):
            sub = os.path.join(rel, name) if rel else name
            path = os.path.join(full, name)
            if os.path.isdir(path):
                entries.append((sub, True))
                walk(sub)
            else:
                with open(path, "rb") as f:
                    data = f.read()
                check_text(sub, data)
                entries.append((sub, False))
                contents[sub] = data

    walk("")
    return entries, contents


# ---- 镜像组装 ---------------------------------------------------------------


def build_image():
    if not os.path.isdir(SRC_TREE):
        raise SystemExit("[mkext2] 找不到种子树 %s" % SRC_TREE)
    entries, contents = collect_tree()
    # e2fsck 惯例：根下要有 /lost+found 空目录（mkfs 的标准配件，不是源树
    # 文件）。缺了它 e2fsck -fn 会自己补建并把 free 计数全改掉，见踩坑 28。
    entries.append(("lost+found", True))

    img = bytearray(TOTAL_BLOCKS * BLOCK_SIZE)

    # inode 分配：根 = 2，其余从 11 起
    ino_of = {"": ROOT_INO}
    next_ino = FIRST_INO
    for rel, is_dir in entries:
        ino_of[rel] = next_ino
        next_ino += 1
    if next_ino > TOTAL_INODES:
        raise SystemExit("[mkext2] inode 不够：%d > %d" % (next_ino, TOTAL_INODES))

    # 块分配：根 37，其余从 38 起；目录 1 块，文件按内容取块
    blk_of = {"": ROOT_BLOCK}
    next_blk = FIRST_DATA
    for rel, is_dir in entries:
        if is_dir:
            blk_of[rel] = next_blk
            next_blk += 1
        else:
            n = max(1, (len(contents[rel]) + BLOCK_SIZE - 1) // BLOCK_SIZE)
            if n > DIRECT_MAX:
                raise SystemExit(
                    "[mkext2] %s 有 %d 块，驱动 v1 只走直接块（上限 %d 块）"
                    % (rel, n, DIRECT_MAX))
            blk_of[rel] = next_blk
            next_blk += n
    if next_blk > TOTAL_BLOCKS:
        raise SystemExit("[mkext2] 块不够：%d > %d" % (next_blk, TOTAL_BLOCKS))

    # 每个目录的直接子项（用于拼目录块和 links_count）
    children = {}
    for rel, is_dir in entries:
        parent = os.path.dirname(rel)
        children.setdefault(parent or "", []).append((os.path.basename(rel), is_dir))
    children.setdefault("", [])
    for rel, is_dir in entries:
        children.setdefault(rel, [])

    # ---- 目录块 ----
    def write_dir_block(dir_rel):
        blk = blk_of[dir_rel]
        off = blk * BLOCK_SIZE
        # "." 指自己，".." 指父目录（根的 ".." 还是根）
        ents = [(".", ino_of[dir_rel]), ("..", ino_of[os.path.dirname(dir_rel) or ""])]
        for name, is_dir in children[dir_rel]:
            sub = os.path.join(dir_rel, name) if dir_rel else name
            ents.append((name, ino_of[sub]))
        pos = off
        for i, (name, ino) in enumerate(ents):
            name_b = name.encode("ascii")
            need = 8 + len(name_b)
            need = (need + 3) & ~3
            if i == len(ents) - 1:
                rec = off + BLOCK_SIZE - pos        # 末条目吃满到块尾
            else:
                rec = need
            assert rec >= 8 and rec % 4 == 0
            struct.pack_into("<IHBB", img, pos, ino, rec, len(name_b), 0)
            img[pos + 8:pos + 8 + len(name_b)] = name_b
            pos += rec
        assert pos == off + BLOCK_SIZE

    write_dir_block("")
    for rel, is_dir in entries:
        if is_dir:
            write_dir_block(rel)

    # ---- 文件内容 ----
    for rel, data in contents.items():
        off = blk_of[rel] * BLOCK_SIZE
        n = (len(data) + BLOCK_SIZE - 1) // BLOCK_SIZE
        img[off:off + len(data)] = data

    # ---- inode 表 ----
    def put_inode(ino, mode, size, links, nblk, blocks):
        off = ITAB_BLOCK0 * BLOCK_SIZE + (ino - 1) * INODE_SIZE
        # ext2 rev0 真布局：i_mode@0 / i_uid@2 / i_size@4 / i_links@0x1A /
        # i_blocks@0x1C / i_block@0x28（i_dtime@0x14 全零=占用中）。
        # （曾按错误记忆把 size 挪到 0x08、links 挪到 0x02——那是 atime/uid 的
        #   地盘，e2fsck 全盘报"已删 inode"才暴露。标准核对要对着 e2fsck 做。）
        struct.pack_into("<H", img, off, mode)                # i_mode
        struct.pack_into("<I", img, off + 0x04, size)         # i_size_lo
        struct.pack_into("<H", img, off + 0x1A, links)        # i_links_count
        struct.pack_into("<I", img, off + 0x1C, nblk * 2)     # i_blocks（512B 扇区）
        for i, b in enumerate(blocks):
            struct.pack_into("<I", img, off + 0x28 + 4 * i, b)
        if len(blocks) > 15:
            raise SystemExit("[mkext2] inode %d 块指针超界" % ino)

    n_sub = sum(1 for rel, is_dir in entries if is_dir and os.path.dirname(rel) == "")
    put_inode(ROOT_INO, ROOT_MODE, BLOCK_SIZE, 2 + n_sub, 1, [ROOT_BLOCK])
    for rel, is_dir in entries:
        ino = ino_of[rel]
        if is_dir:
            sub_n = sum(1 for c, d in children[rel] if d)
            put_inode(ino, ROOT_MODE, BLOCK_SIZE, 2 + sub_n, 1, [blk_of[rel]])
        else:
            data = contents[rel]
            n = max(1, (len(data) + BLOCK_SIZE - 1) // BLOCK_SIZE)
            blocks = [blk_of[rel] + i for i in range(n)]
            put_inode(ino, FILE_MODE, len(data), 1, n, blocks)

    # ---- 位图 ----
    used_blocks = set([0, SB_BLOCK, GDT_BLOCK, BBM_BLOCK, IBM_BLOCK])
    used_blocks.update(range(ITAB_BLOCK0, ITAB_BLOCK0 + ITAB_BLOCKS))
    used_blocks.update(blk_of.values())
    for rel, data in contents.items():
        n = max(1, (len(data) + BLOCK_SIZE - 1) // BLOCK_SIZE)
        used_blocks.update(range(blk_of[rel], blk_of[rel] + n))
    # 标准（first_data_block=1 的 1KB 盘）：块位图 bit i ↔ 块 i+1，块 0
    # （引导区）不占位图。曾写成 bit N ↔ 块 N，e2fsck 读成"块 43 假占用"。
    for b in sorted(used_blocks):
        if b == 0:
            continue
        img[BBM_BLOCK * BLOCK_SIZE + (b - 1) // 8] |= 1 << ((b - 1) % 8)

    used_inodes = set(range(1, 11))          # 1..10 保留（含根 2）
    used_inodes.update(ino_of.values())
    for i in sorted(used_inodes):
        # 标准：inode N ↔ 位图 bit N-1（bit 0 = inode 1）。曾写成 bit N，整体差 1。
        img[IBM_BLOCK * BLOCK_SIZE + (i - 1) // 8] |= 1 << ((i - 1) % 8)

    # 位图尾 padding（e2fsck 约定，不计入 free 计数）：
    #   inode 位图：bit >= inodes_count 的位全置 1（这盘 256 个 inode → byte 32 起全 FF）；
    #   块位图：最后一位置 1（e2fsck 1.47.3 对 1KB 块盘的行为，bit 8191）。
    # 运行时位图分配器扫 0 位不受影响（空闲位都在 padding 之前）。
    for off in range(IBM_BLOCK * BLOCK_SIZE + (TOTAL_INODES + 7) // 8,
                     (IBM_BLOCK + 1) * BLOCK_SIZE):
        img[off] = 0xFF
    img[BBM_BLOCK * BLOCK_SIZE + (TOTAL_BLOCKS - 1) // 8] |= \
        1 << ((TOTAL_BLOCKS - 1) % 8)

    # free 计数按 e2fsck 的口径 = blocks_count - 位图置位数。置位数里：
    #   真占用 = len(used_blocks) - 1（块 0 不占位图）；
    #   padding 位（8191）也置 1 且算占用。
    # 两数互抵 → free = TOTAL_BLOCKS - len(used_blocks)，数值上与不移位时一致。
    free_blocks = TOTAL_BLOCKS - len(used_blocks)
    free_inodes = TOTAL_INODES - len(used_inodes)
    n_dirs = 1 + sum(1 for rel, is_dir in entries if is_dir)

    # ---- 超块（块 1，字节偏移 1024）----
    sb = SB_BLOCK * BLOCK_SIZE
    struct.pack_into("<I", img, sb + 0x00, TOTAL_INODES)       # s_inodes_count
    struct.pack_into("<I", img, sb + 0x04, TOTAL_BLOCKS)       # s_blocks_count
    struct.pack_into("<I", img, sb + 0x08, 0)                  # s_r_blocks_count
    struct.pack_into("<I", img, sb + 0x0C, free_blocks)        # s_free_blocks_count
    struct.pack_into("<I", img, sb + 0x10, free_inodes)        # s_free_inodes_count
    struct.pack_into("<I", img, sb + 0x14, 1)                  # s_first_data_block
    struct.pack_into("<I", img, sb + 0x18, 0)                  # s_log_block_size = 0（1024B）
    struct.pack_into("<I", img, sb + 0x1C, 0)                  # s_log_frag_size
    struct.pack_into("<I", img, sb + 0x20, TOTAL_BLOCKS)       # s_blocks_per_group
    struct.pack_into("<I", img, sb + 0x24, TOTAL_BLOCKS)       # s_frags_per_group
    struct.pack_into("<I", img, sb + 0x28, TOTAL_INODES)       # s_inodes_per_group
    struct.pack_into("<H", img, sb + 0x38, EXT2_MAGIC)         # s_magic
    struct.pack_into("<H", img, sb + 0x3A, 1)                  # s_state = 1 clean
    struct.pack_into("<H", img, sb + 0x3C, 0)                  # s_errors
    struct.pack_into("<I", img, sb + 0x4C, 0)                  # s_rev_level = 0
    struct.pack_into("<I", img, sb + 0x60, 0)                  # s_feature_incompat（rev0 为 0）

    # ---- 组描述符（块 2 第 0 项）----
    gd = GDT_BLOCK * BLOCK_SIZE
    struct.pack_into("<I", img, gd + 0x00, BBM_BLOCK)          # bg_block_bitmap
    struct.pack_into("<I", img, gd + 0x04, IBM_BLOCK)          # bg_inode_bitmap
    struct.pack_into("<I", img, gd + 0x08, ITAB_BLOCK0)        # bg_inode_table
    struct.pack_into("<H", img, gd + 0x0C, free_blocks)        # bg_free_blocks_count
    struct.pack_into("<H", img, gd + 0x0E, free_inodes)        # bg_free_inodes_count
    struct.pack_into("<H", img, gd + 0x10, n_dirs)             # bg_used_dirs_count

    # ---- 自检（四面对账，见模块 docstring）----
    selfcheck(img, entries, contents, used_blocks, used_inodes,
              free_blocks, free_inodes, n_dirs)

    return bytes(img)


# ---- 自检 -------------------------------------------------------------------


def selfcheck(img, entries, contents, used_blocks, used_inodes,
              free_blocks, free_inodes, n_dirs):
    ok = True

    # 2. 位图逐位对账（独立重扫，不信 build_image 里的集合）
    # 块位图：bit i ↔ 块 i+1（块 0 不占位图）；bit 8191 是 padding
    # （e2fsck 约定，不是占用），从对账范围里剔出去、单独断言。
    bm = set()
    base = BBM_BLOCK * BLOCK_SIZE
    for i in range(TOTAL_BLOCKS - 1):
        if img[base + i // 8] >> (i % 8) & 1:
            bm.add(i + 1)                    # bit i → 块 i+1
    if bm != used_blocks - {0}:
        print("[mkext2][自检] 块位图与占用集合不符：%s"
              % (sorted(bm ^ (used_blocks - {0}))[:8]))
        ok = False
    if not (img[base + (TOTAL_BLOCKS - 1) // 8] >> ((TOTAL_BLOCKS - 1) % 8)) & 1:
        print("[mkext2][自检] 块位图 padding 位（%d）没置 1" % (TOTAL_BLOCKS - 1))
        ok = False
    im = set()
    base = IBM_BLOCK * BLOCK_SIZE
    for b in range(TOTAL_INODES):
        if img[base + b // 8] >> (b % 8) & 1:
            im.add(b + 1)                    # 标准：bit b ↔ inode b+1
    if im != used_inodes:
        print("[mkext2][自检] inode 位图与分配集合不符：%s"
              % (sorted(im ^ used_inodes)[:8]))
        ok = False

    # 4. free 计数
    sb = SB_BLOCK * BLOCK_SIZE
    if struct.unpack_from("<I", img, sb + 0x0C)[0] != free_blocks or \
       struct.unpack_from("<I", img, sb + 0x10)[0] != free_inodes:
        print("[mkext2][自检] 超块 free 计数不符")
        ok = False
    gd = GDT_BLOCK * BLOCK_SIZE
    if struct.unpack_from("<H", img, gd + 0x0C)[0] != free_blocks or \
       struct.unpack_from("<H", img, gd + 0x0E)[0] != free_inodes or \
       struct.unpack_from("<H", img, gd + 0x10)[0] != n_dirs:
        print("[mkext2][自检] 组描述符计数不符")
        ok = False

    # 1 + 3. 独立重放整棵树（只信超块/GD/inode/目录链，不信分配过程）
    seen_files = {}

    def walk_dir(ino, rel):
        nonlocal ok
        for blk in iter_direct_blocks(img, ino):
            off = blk * BLOCK_SIZE
            pos = 0
            while pos < BLOCK_SIZE:
                e_ino, rec, nlen, ftype = struct.unpack_from("<IHBB", img, off + pos)
                if rec < 8 or rec % 4 != 0 or pos + rec > BLOCK_SIZE or \
                        8 + nlen > rec:
                    print("[mkext2][自检] %s 目录项坏：off=%d rec=%d nlen=%d"
                          % (rel, pos, rec, nlen))
                    ok = False
                    return
                name = bytes(img[off + pos + 8:off + pos + 8 + nlen]).decode(
                    "ascii", "replace")
                if e_ino != 0 and name not in (".", ".."):
                    if e_ino in seen_files:
                        print("[mkext2][自检] inode %d 被两个路径引用" % e_ino)
                        ok = False
                    child = os.path.join(rel, name) if rel else name
                    mode, size = inode_mode_size(img, e_ino)
                    if mode >> 12 == 0x4:
                        walk_dir(e_ino, child)
                    elif mode >> 12 == 0x8:
                        seen_files[child] = read_file_bytes(img, e_ino, size)
                    else:
                        print("[mkext2][自检] %s 的 inode %d mode 0x%X 不是文件/目录"
                              % (child, e_ino, mode))
                        ok = False
                pos += rec

    walk_dir(ROOT_INO, "")

    if seen_files != contents:
        keys = set(seen_files) | set(contents)
        bad = [k for k in sorted(keys) if seen_files.get(k) != contents.get(k)]
        print("[mkext2][自检] 独立重放与源树不符：%s" % bad)
        ok = False

    if not ok:
        raise SystemExit("[mkext2] 自检失败，镜像不可信")
    print("[mkext2] 自检 4 面全过：重放/块位图/inode 位图/free 计数")


def iter_direct_blocks(img, ino):
    """只走直接块 i_block[0..11]（v1 驱动同款约束），到 0 为止。"""
    off = ITAB_BLOCK0 * BLOCK_SIZE + (ino - 1) * INODE_SIZE
    for i in range(12):
        b = struct.unpack_from("<I", img, off + 0x28 + 4 * i)[0]
        if b == 0:
            return
        yield b


def inode_mode_size(img, ino):
    off = ITAB_BLOCK0 * BLOCK_SIZE + (ino - 1) * INODE_SIZE
    mode = struct.unpack_from("<H", img, off)[0]
    size = struct.unpack_from("<I", img, off + 0x04)[0]   # i_size_lo（rev0 真偏移）
    return mode, size


def read_file_bytes(img, ino, size):
    out = bytearray()
    for blk in iter_direct_blocks(img, ino):
        out += img[blk * BLOCK_SIZE:blk * BLOCK_SIZE + BLOCK_SIZE]
    return bytes(out[:size])


# ---- VMDK 包装 --------------------------------------------------------------


def write_vmdk(img_path, flat_path, n_sectors):
    desc = (
        "# Disk DescriptorFile\n"
        "version=1\n"
        "CID=fffffffe\n"
        "parentCID=ffffffff\n"
        # createType 必须是 monolithicFlat（描述符 + 单个 -flat.vmdk 数据文件）。
        # 方案初稿写的 twoFileMaxExtent 不是合法类型，VMware 报
        # "Unknown disk create type"直接拒挂——实测教训（§11 踩坑 5）。
        "createType=\"monolithicFlat\"\n"
        "\n"
        "# Extent description\n"
        "RW %d FLAT \"%s\" 0\n"
        "\n"
        "# The Disk Data Base\n"
        "#DDB\n"
        "\n"
        "ddb.adapterType = \"ide\"\n"
        "ddb.geometry.cylinders = \"17\"\n"
        "ddb.geometry.heads = \"16\"\n"
        "ddb.geometry.sectors = \"63\"\n"
        "ddb.toolsVersion = \"0\"\n"
    ) % (n_sectors, os.path.basename(flat_path))
    with open(img_path, "w", newline="") as f:
        f.write(desc)


def main():
    os.makedirs(BUILD_DIR, exist_ok=True)

    if len(sys.argv) >= 3 and sys.argv[1] == "--from-mke2fs":
        src = sys.argv[2]
        with open(src, "rb") as f:
            n = len(f.read())
        if n % 512:
            raise SystemExit("[mkext2] 镜像不是 512 的倍数")
        flat = os.path.join(BUILD_DIR, "hd-flat.vmdk")
        write_vmdk(os.path.join(BUILD_DIR, "hd.vmdk"), flat, n // 512)
        print("[mkext2] --from-mke2fs：%s -> hd.vmdk（%d 扇区，不生成镜像）"
              % (src, n // 512))
        return

    img = build_image()
    raw = os.path.join(BUILD_DIR, "hd-ext2.img")
    flat = os.path.join(BUILD_DIR, "hd-flat.vmdk")
    with open(raw, "wb") as f:
        f.write(img)
    with open(flat, "wb") as f:
        f.write(img)
    write_vmdk(os.path.join(BUILD_DIR, "hd.vmdk"), flat, len(img) // 512)
    print("[mkext2] hd-ext2.img / hd-flat.vmdk %d B（%d 扇区），hd.vmdk 描述符已写"
          % (len(img), len(img) // 512))


if __name__ == "__main__":
    main()
