#!/usr/bin/env python3
"""把 src/ramfs/ 目录打包成 MeFS 镜像，生成 src/kernel/ramfs.inc。

MeFS 是一个 16MB 的 RAM 文件系统，布局（每块 512 字节）：
    块 0       超块：魔数 / 版本 / 块大小 / 总块数 / 空闲块数 / 根目录块
    块 1..8    分配位图（FS_BITMAP_BLOCKS 块，共 FS_BLOCKS 个位）
    块 9..     数据

每块前 4 字节是「链上下一块」（0 = 结尾），后 508 字节是数据。
目录的数据是一串 42 字节的记录：
    kind(1) name_len(1) name(32) first_block(4) size(4)
文件按内容长度链式排块；目录的 size 记子项条数（不是字节数）。

镜像里的块号 = 运行时 RAM 里的块号：快照从 0x00100000 原样拷过去，
fs.inc 直接按块号寻址。所以这里必须按 512 字节一列排好，不能偷懒。

src/ramfs/ 里只允许可打印 ASCII 和换行：终端的字模只有 0x20..0x7E。
生成物是 src/kernel/ramfs.inc（和 genfont.py 一样的「构建时现烤」风格）。

生成完会自检一遍：把镜像按块解析，核对每个目录记录的 first_block
在位图里确实被标记、且指向块的内容和源文件一致。对不上直接报错退出，
避免坏镜像混进构建（踩过：记录字段偏移写错，生成物看着像模像样，
一读全是乱码）。

用法：python tools/mkramfs.py
"""

import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_RAMFS = os.path.join(ROOT, "src", "ramfs")
OUT_INC = os.path.join(ROOT, "src", "kernel", "ramfs.inc")

FS_BLOCK_SIZE = 512
FS_RAM_SIZE = 16 * 1024 * 1024
FS_BLOCKS = FS_RAM_SIZE // FS_BLOCK_SIZE              # 32768
FS_BITMAP_BLOCKS = (FS_BLOCKS // 8) // FS_BLOCK_SIZE  # 8
FS_ROOT_DEFAULT = 1 + FS_BITMAP_BLOCKS                # 9（不写死，超块里记真的）
FS_MAGIC = 0x5346454D                                # "MEFS"

BLK_NEXT, BLK_DATA = 0, 4
BLK_CAP = FS_BLOCK_SIZE - BLK_DATA                    # 508

DE_KIND, DE_NAME_LEN, DE_NAME = 0, 1, 2
DE_FIRST, DE_SIZE = 0x22, 0x26                        # 名字 32 字节，从 34 开始才是 first
DE_LEN = 42
DE_PER_BLK = BLK_CAP // DE_LEN                        # 12
MAX_NAME = 31

KIND_FILE, KIND_DIR = 0, 1


def check_text(data, where):
    for ch in data:
        if ch == 10:                        # 换行允许
            continue
        if not (0x20 <= ch <= 0x7E):
            raise SystemExit("%s 只允许可打印 ASCII 与换行，发现 0x%02X" % (where, ch))


def main():
    blocks = {}
    bitmap = bytearray(FS_BITMAP_BLOCKS * FS_BLOCK_SIZE)
    allocated = 0

    def mark(b):
        nonlocal allocated
        bitmap[b // 8] |= 1 << (b % 8)
        allocated += 1

    for b in range(FS_ROOT_DEFAULT):       # 超块 + 位图
        mark(b)

    def alloc_block():
        for b in range(FS_ROOT_DEFAULT, FS_BLOCKS):
            if not (bitmap[b // 8] & (1 << (b % 8))):
                mark(b)
                return b
        raise SystemExit("ramfs 快照超出可用块数")

    def make_blob(payload):
        blob = bytearray(FS_BLOCK_SIZE)
        blob[BLK_DATA:BLK_DATA + len(payload)] = payload
        return blob

    def alloc_chain(payloads):
        """把一串 ≤508 字节的载荷排成链，返回首块号（空链返回 0）。"""
        if not payloads:
            return 0
        blks = [alloc_block() for _ in payloads]
        for b, payload in zip(blks, payloads):
            blocks[b] = make_blob(payload)
        for a, b in zip(blks, blks[1:]):
            struct.pack_into("<I", blocks[a], BLK_NEXT, b)
        return blks[0]

    def encode_dir(entries):
        payload = bytearray()
        for kind, name, first, size in entries:
            raw = name.encode("ascii")
            if len(raw) > MAX_NAME:
                raise SystemExit("名字太长：%s" % name)
            rec = bytearray(DE_LEN)
            rec[DE_KIND] = kind
            rec[DE_NAME_LEN] = len(raw)
            rec[DE_NAME:DE_NAME + len(raw)] = raw
            struct.pack_into("<II", rec, DE_FIRST, first, size)
            payload += rec
        return payload

    def walk(path, name):
        """深度优先收编：先处理子树（分配它们的块），再给本目录排记录。"""
        if os.path.isdir(path):
            children = []
            for ent in sorted(os.listdir(path)):
                children.append(walk(os.path.join(path, ent), ent))
            payload = encode_dir(children)
            first = alloc_chain([payload[i:i + BLK_CAP]
                                 for i in range(0, len(payload), BLK_CAP)])
            return (KIND_DIR, name, first, len(children))
        else:
            with open(path, "rb") as fp:
                data = fp.read()
            check_text(data, path)
            payloads = [data[i:i + BLK_CAP]
                        for i in range(0, len(data), BLK_CAP)]
            first = alloc_chain(payloads)
            return (KIND_FILE, name, first, len(data))

    root_entries = []
    for ent in sorted(os.listdir(SRC_RAMFS)):
        child = os.path.join(SRC_RAMFS, ent)
        if os.path.isdir(child) or os.path.isfile(child):
            root_entries.append(walk(child, ent))

    # 根目录的块要等所有子项都分配完再分配，否则会盖掉子项数据
    # （踩过：一开始把根直接塞进固定的块 9，hello.txt 的数据被目录记录覆盖）。
    root_payload = encode_dir(root_entries)
    root_first = alloc_chain([root_payload[i:i + BLK_CAP]
                              for i in range(0, len(root_payload), BLK_CAP)])
    if root_first == 0:
        raise SystemExit("根目录为空，无法打包")

    # ---- 超块 ----
    free_count = FS_BLOCKS - allocated
    sb = bytearray(FS_BLOCK_SIZE)
    struct.pack_into("<6I", sb, 0, FS_MAGIC, 1, FS_BLOCK_SIZE,
                     FS_BLOCKS, free_count, root_first)
    blocks[0] = sb
    # 位图按块分开存，保证「镜像块号 = RAM 块号」
    for i in range(FS_BITMAP_BLOCKS):
        blocks[1 + i] = bytes(bitmap[i * FS_BLOCK_SIZE:(i + 1) * FS_BLOCK_SIZE])

    # ---- 拼镜像 ----
    last = max(blocks)
    image = bytearray()
    for b in range(last + 1):
        image += blocks.get(b, bytearray(FS_BLOCK_SIZE))

    # ---- 自检：逐块解析回去，确认记录字段、位图、内容三面对得上 ----
    def block_bytes(n):
        off = n * FS_BLOCK_SIZE
        if off + FS_BLOCK_SIZE > len(image):
            return None
        return image[off:off + FS_BLOCK_SIZE]

    def chain_bytes(first):
        out = bytearray()
        b = first
        seen = 0
        while b != 0:
            if b < 0 or b >= FS_BLOCKS or seen > FS_BLOCKS:
                raise SystemExit("自检：块链成环或越界（first=%d）" % first)
            bb = block_bytes(b)
            if bb is None:
                raise SystemExit("自检：块 %d 超出镜像" % b)
            out += bb[BLK_DATA:]
            b = struct.unpack_from("<I", bb, BLK_NEXT)[0]
            seen += 1
        return bytes(out)

    def check_tree(first, expected_dir, where):
        """expected_dir: dict name->(kind,size) 或 None 表示只要是个文件。"""
        for sl in range(last + 1):
            bb = block_bytes(sl)
            if not bb:
                break
            if bb[BLK_NEXT] != 0:
                pass
        payload = chain_bytes(first)
        if expected_dir is None:
            return
        # 目录：解析记录，逐个核对
        n = 0
        while n + DE_LEN <= len(payload):
            rec = payload[n:n + DE_LEN]
            nl = rec[DE_NAME_LEN]
            if nl == 0:
                break
            kind = rec[DE_KIND]
            name = bytes(rec[DE_NAME:DE_NAME + nl]).decode("ascii")
            f = struct.unpack_from("<I", rec, DE_FIRST)[0]
            size = struct.unpack_from("<I", rec, DE_SIZE)[0]
            if name not in expected_dir:
                raise SystemExit("自检：%s 出现意外记录 %r" % (where, name))
            ekind, esize = expected_dir[name]
            if kind != ekind or size != esize:
                raise SystemExit("自检：%s/%s 属性不符 kind=%d size=%d（期望 %d/%d）"
                                 % (where, name, kind, size, ekind, esize))
            if not (bitmap[f // 8] & (1 << (f % 8))):
                raise SystemExit("自检：%s/%s 的首块 %d 没有在位图里" % (where, name, f))
            if ekind == KIND_FILE:
                content = chain_bytes(f)
                src = os.path.join(SRC_RAMFS, where, name) if where else \
                    os.path.join(SRC_RAMFS, name)
                with open(src, "rb") as fp:
                    expect = fp.read()
                if content[:len(expect)] != expect:
                    raise SystemExit("自检：%s/%s 内容对不上" % (where, name))
            n += DE_LEN

    check_tree(root_first, {e[1]: (e[0], e[3]) for e in root_entries}, "")

    # ---- 输出 .inc ----
    lines = ["; 本文件由 tools/mkramfs.py 自动生成，请勿手工修改",
             "; MeFS 快照：来自 src/ramfs/，布局见 mkramfs.py 头注释",
             "ramfs_snapshot_len equ %d" % len(image),
             "ramfs_snapshot:"]
    for i in range(0, len(image), 16):
        row = image[i:i + 16]
        lines.append("    db " + ",".join("0x%02X" % b for b in row))
    with open(OUT_INC, "w", encoding="utf-8", newline="\n") as fp:
        fp.write("\n".join(lines) + "\n")

    print("[mkramfs] %s -> %d bytes / %d blocks used / %d free"
          % (os.path.basename(OUT_INC), len(image), allocated, free_count))
    return 0


if __name__ == "__main__":
    sys.exit(main())
