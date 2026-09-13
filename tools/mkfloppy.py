#!/usr/bin/env python3
"""把引导扇区和内核载荷拼成一张 1.44MB 软盘镜像。

镜像布局（LBA 编号，每扇区 512 字节）：
    扇区 0      引导扇区（boot.bin，恰好 512 字节，结尾 0x55AA）
    扇区 1..    内核载荷（payload.bin，从物理地址 0x8000 开始连续装入）

这张软盘镜像有两个用途：
    1. 直接作为 VMware 的软盘设备启动；
    2. 作为 El Torito 引导镜像，交给 mkisofs 做成可引导 ISO。

用法：
    python tools/mkfloppy.py <boot.bin> <payload.bin|-> <out.img>
    第二个参数写 "-" 表示暂时没有内核载荷。
"""

import os
import sys

SECTOR_SIZE = 512
FLOPPY_SIZE = 1474560                      # 1.44MB = 2880 个扇区


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2

    boot_path, payload_path, out_path = sys.argv[1:4]

    with open(boot_path, "rb") as fp:
        boot = fp.read()
    if len(boot) != SECTOR_SIZE:
        raise SystemExit("引导扇区必须是 %d 字节，实际 %d 字节" % (SECTOR_SIZE, len(boot)))
    if boot[-2:] != b"\x55\xaa":
        raise SystemExit("引导扇区结尾缺少 0x55 0xAA 签名")

    payload = b""
    if payload_path != "-":
        if not os.path.exists(payload_path):
            raise SystemExit("找不到内核载荷：" + payload_path)
        with open(payload_path, "rb") as fp:
            payload = fp.read()
        if len(payload) % SECTOR_SIZE:
            payload += b"\x00" * (SECTOR_SIZE - len(payload) % SECTOR_SIZE)

    used = SECTOR_SIZE + len(payload)
    if used > FLOPPY_SIZE:
        raise SystemExit("载荷过大：需要 %d 字节，软盘只有 %d 字节" % (used, FLOPPY_SIZE))

    with open(out_path, "wb") as fp:
        fp.write(boot)
        fp.write(payload)
        fp.write(b"\x00" * (FLOPPY_SIZE - used))

    print("[mkfloppy] %s -> 引导扇区 %dB + 载荷 %dB / 共 %dB"
          % (os.path.basename(out_path), SECTOR_SIZE, len(payload), FLOPPY_SIZE))
    return 0


if __name__ == "__main__":
    sys.exit(main())