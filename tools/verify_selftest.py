#!/usr/bin/env python3
"""验证自检版 MeOS 的整屏渲染。

读 .vmem 里内核数出来的「整屏白像素数」，再在宿主机这边把同一场会话按同样的
规则重算一遍，两边一比：

  · 对得上 -> 显存里该有的东西一个像素不差，剩下的问题只可能在显示刷新那一段；
  · 对不上 -> 要么字模错，要么布局（换行 / 折行 / 滚屏）错。

会话内容一律从 kernel.asm / ascii.inc 里读（自检扫描码流、键盘码表、提示符、
各条命令的输出字符串），不在这儿另抄一份——抄一份就等于埋一个「改了 A 忘了 B」。

用法：python tools/verify_selftest.py [vmem 路径]
"""

import glob
import io
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, "src", "kernel", "kernel.asm")
DIAG_ADDR, PARAM_ADDR = 0x6200, 0x6000
D_CONPIX = 0x84
D_ROWPIX = 0x88


def read_src(path):
    with io.open(path, "r", encoding="utf-8", errors="replace") as fp:
        return fp.read()


def num(tok):
    return int(tok, 16) if tok.lower().startswith("0x") else int(tok)


def split_db(body):
    """把 db 后面的内容切成一个个值。

    不能用 body.split(",") 简单了事：码表里有 ',' 这种「引号里带逗号」的字面量，
    一 split 就散架了。
    """
    vals, i, n = [], 0, len(body)
    while i < n:
        c = body[i]
        if c in " \t,":
            i += 1
        elif c == "'":
            j = i + 1
            if j < n and body[j] == "\\":
                j += 2
            else:
                j += 1
            vals.append(ord(body[i + 1:j]))
            i = j + 1
        else:
            m = re.match(r"[0-9A-Fa-fxX]+", body[i:])
            if not m:
                break
            vals.append(num(m.group(0)))
            i += m.end()
    return vals


def parse_bytes(text, label):
    """把 label: 后面的 db 列表拼成字节串，遇到下一个顶层标签就停。"""
    lines = text.split("\n")
    start = next(i for i, ln in enumerate(lines) if ln.strip() == label + ":")
    out = []
    for ln in lines[start + 1:]:
        t = ln.strip()
        if not t or t.startswith(";"):
            continue
        if re.match(r"^[A-Za-z_.][\w.]*:", t):      # 下一个标签，收工
            break
        m = re.match(r"times\s+(\d+)\s+db\s+(\S+)", t)
        if m:
            out.extend([num(m.group(2))] * int(m.group(1)))
            continue
        if not t.startswith("db "):
            break
        out.extend(split_db(t[3:]))
    return bytes(out)


def eq_value(text, name):
    m = re.search(r"^%s\s+equ\s+(\S+)" % re.escape(name), text, re.M)
    if not m:
        raise SystemExit("kernel.asm 里找不到 %s" % name)
    return num(m.group(1))


def db_strings(text):
    """抓 msg_xxx db "..." 这类常量字符串。"""
    out = {}
    for m in re.finditer(r'^(msg_\w+)\s+db\s+"([^"]*)"', text, re.M):
        out[m.group(1)] = m.group(2)
    return out


def find_vmem():
    if len(sys.argv) > 1:
        return sys.argv[1]
    hits = glob.glob(os.path.join(ROOT, "build", "*.vmem"))
    if not hits:
        raise SystemExit("找不到 .vmem，虚拟机是不是没在跑？")
    return hits[0]


def main():
    asm = read_src(ASM)
    ascii_inc = read_src(os.path.join(ROOT, "src", "kernel", "ascii.inc"))
    font_inc = read_src(os.path.join(ROOT, "src", "kernel", "font.inc"))

    # ---- 字模：每个字符一共点亮多少像素 ----
    cell_w, cell_h = eq_value(ascii_inc, "ASCII_CELL_W"), eq_value(ascii_inc, "ASCII_CELL_H")
    first, count = eq_value(ascii_inc, "ASCII_FIRST"), eq_value(ascii_inc, "ASCII_COUNT")
    glyph_len = eq_value(ascii_inc, "ASCII_GLYPH_LEN")
    ascii_raw = parse_bytes(ascii_inc, "ascii_font")
    assert len(ascii_raw) == count * glyph_len, len(ascii_raw)
    bits = {}
    for i in range(count):
        chunk = ascii_raw[i * glyph_len:(i + 1) * glyph_len]
        bits[chr(first + i)] = sum(bin(b).count("1") for b in chunk)
    font_raw = parse_bytes(font_inc, "font_bitmap")
    banner_bits = sum(bin(b).count("1") for b in font_raw)

    # ---- MeFS 快照：pwd / ls / cat 的输出从它里面读 --------------------------
    #  布局和 mkramfs.py / fs.inc 里的约定是同一份：块 512B，首 4 字节 = 链上
    #  下一块，目录记录 42 字节（kind/name_len/name(32)/first(4)/size(4)）。
    ramfs_inc = read_src(os.path.join(ROOT, "src", "kernel", "ramfs.inc"))
    snap = parse_bytes(ramfs_inc, "ramfs_snapshot")

    def fs_block(n):
        off = n * 512
        return snap[off:off + 512]

    def fs_dir_entries(block):
        """按链顺序列出目录项 [(kind, name, first, size)]。"""
        out = []
        blk = block
        while blk:
            data = fs_block(blk)
            nxt = struct.unpack_from("<I", data, 0)[0]
            for off in range(4, 512 - 42 + 1, 42):
                rec = data[off:off + 42]
                nl = rec[1]
                if nl == 0:
                    continue
                out.append((rec[0], rec[2:2 + nl].decode("ascii"),
                            struct.unpack_from("<I", rec, 0x22)[0],
                            struct.unpack_from("<I", rec, 0x26)[0]))
            blk = nxt
        return out

    def fs_read_file(first, size):
        out = bytearray()
        blk = first
        while size > 0 and blk:
            data = fs_block(blk)
            take = min(size, 508)
            out += data[4:4 + take]
            size -= take
            blk = struct.unpack_from("<I", data, 0)[0]
        return bytes(out)

    fs_root_block = struct.unpack_from("<I", snap, 0x14)[0]

    # ---- VFS 的静态事实，从 vfs.inc 里读回来 --------------------------------
    #  挂载表 / 文件系统名 / 设备注册表 / 类型名，都是「数组里 dd 符号名 + 底下
    #  db 字符串」两部分。跟内核一样表驱动，不在这另抄一份行为。
    vfs_inc = read_src(os.path.join(ROOT, "src", "kernel", "vfs.inc"))

    def follow_dd_table(text, label):
        """抓 `label:` 后面连续 `dd 符号名` 形式的条目，到不是这种行就停。"""
        lines = text.split("\n")
        start = next(i for i, ln in enumerate(lines) if ln.strip() == label + ":")
        names = []
        for ln in lines[start + 1:]:
            t = ln.strip()
            if not t or t.startswith(";"):
                continue
            m = re.match(r"^dd\s+([A-Za-z_]\w*)", t)
            if not m:
                break
            names.append(m.group(1))
        return names

    def db_label_strings(text):
        out = {}
        for m in re.finditer(r'^([A-Za-z_]\w*)\s+db\s+"([^"]*)"', text, re.M):
            out[m.group(1)] = m.group(2)
        return out

    vfs_labels = db_label_strings(vfs_inc)
    mnts = [vfs_labels[s] for s in follow_dd_table(vfs_inc, "vfs_mount_tab")]
    fsnames = [vfs_labels[s] for s in follow_dd_table(vfs_inc, "vfs_fsname_tab")]
    devnames = [vfs_labels[s] for s in follow_dd_table(vfs_inc, "devfs_ent")]
    type_names = [vfs_labels[s] for s in follow_dd_table(vfs_inc, "vfs_type_msgs")]
    cm = re.search(r'^devfs_console_text\s+db\s+"([^"]*)"([^\n]*)', vfs_inc, re.M)
    console_text = cm.group(1) + ("\n" if "10" in cm.group(2) else "")
    assert mnts and fsnames and devnames, "vfs.inc 解析失败"

    # ---- 宿主机这边维护一棵可变文件树，模拟 touch/mkdir/cp/mv/rm 的副作用 ----
    #  快照里的内容整个读进来当种子，之后写命令的操作就发生在这棵树上，
    #  ls / cat / stat 都从它取数 —— 这样 "touch 建的 /t1.txt" 才解得开。
    def load_tree(block):
        d = {}
        for kind, name, first, size in fs_dir_entries(block):
            if kind == 1:
                d[name] = ("dir", load_tree(first))
            else:
                d[name] = ("file", fs_read_file(first, size))
        return d

    root_dir = load_tree(fs_root_block)

    # ---- ext2 硬盘模型：从 build/hd-ext2.img 直接解析一棵只读树 --------------
    #  磁上的产物就是地面真值；目录顺序按磁盘顺序保留（dict 插入序 = 遍历序），
    #  ls /hd 的输出顺序跟内核 ext2_readdir 的目录块遍历天然一致。
    #  FS3 只支持直接块（12 块），这棵树也只走直接块——两边能力对齐。
    e2root = {}
    e2sz = {}
    with open(os.path.join(ROOT, "build", "hd-ext2.img"), "rb") as _fp:
        _fp.seek(1024)                   # 超块在块 1（1024B 盘），不在块 0
        _sb = _fp.read(1024)
        assert _sb[0x38:0x3A] == b"\x53\xEF", "ext2 魔数不对"
        _bs = 1024 << struct.unpack("<I", _sb[0x18:0x1C])[0]
        _fp.seek(2 * _bs)
        _gd = _fp.read(_bs)
        _inotab = struct.unpack("<I", _gd[8:12])[0]

        def _e2_walk(ino, path):
            _fp.seek(_inotab * _bs + (ino - 1) * 128)
            i = _fp.read(128)
            size = struct.unpack("<I", i[0x04:0x08])[0]   # i_size_lo（rev0 真偏移）
            mode = struct.unpack("<H", i[0x00:0x02])[0]
            blocks = struct.unpack("<12I", i[0x28:0x58])
            data = b""
            for b in blocks:
                if b:
                    _fp.seek(b * _bs)
                    data += _fp.read(_bs)
            data = data[:size]
            e2sz[path] = size
            if (mode & 0xF000) != 0x4000:
                return data
            d = {}
            off = 0
            while off + 8 <= len(data):
                e_ino, rec = struct.unpack("<IH", data[off:off + 6])
                nlen = data[off + 6]
                if rec < 8 or rec % 4:
                    break
                if e_ino and nlen and off + 8 + nlen <= len(data):
                    name = data[off + 8:off + 8 + nlen].decode("latin-1")
                    if name not in (".", ".."):
                        d[name] = _e2_walk(e_ino, path + "/" + name)
                off += rec
            return d

        e2root = _e2_walk(2, "")

    def e2_lookup(rel):
        """rel 是相对挂载点 /hd 的路径（"" = 根）。返回 dict（目录）或 bytes（文件）。"""
        node = e2root
        for c in [c for c in rel.split("/") if c]:
            if not isinstance(node, dict) or c not in node:
                return None
            node = node[c]
        return node

    def _e2_kindsize(rel):
        node = e2_lookup(rel)
        if node is None:
            return None
        if isinstance(node, dict):
            return (1, e2sz.get(rel, 1024))
        return (0, len(node))

    def tree_node(path):
        """逐级下落。返回 (父 dict, 名字, (kind, payload)) 或 None。"""
        comps = [c for c in path.split("/") if c]
        cur = root_dir
        for c in comps[:-1]:
            if not isinstance(cur, dict) or c not in cur or cur[c][0] != "dir":
                return None
            cur = cur[c][1]
        if not comps or not isinstance(cur, dict) or comps[-1] not in cur:
            return None
        return (cur, comps[-1], cur[comps[-1]])

    def tree_resolve(path):
        t = tree_node(path)
        if not t:
            return None
        kind, payload = t[2]
        if kind == "file":
            return (0, len(payload), payload)
        return (1, len(payload), None)

    def tree_parent(path):
        """返回 (父目录 dict, 名字) 或 None。"""
        comps = [c for c in path.split("/") if c]
        cur = root_dir
        for c in comps[:-1]:
            if not isinstance(cur, dict) or c not in cur or cur[c][0] != "dir":
                return None
            cur = cur[c][1]
        if not isinstance(cur, dict):
            return None
        return (cur, comps[-1] if comps else None)

    def vfs_resolve(path):
        """按挂载表最长前缀把路径拆给驱动，返回 (drv, rel, (kind, size))。"""
        best, drv = -1, None
        for i, mnt in enumerate(mnts):
            if mnt == "/":
                mlen = 1                 # 根挂载：任何绝对路径都中（和内核一致）
            elif path == mnt or path.startswith(mnt + "/"):
                mlen = len(mnt)
            else:
                continue
            if mlen > best:
                best, drv = mlen, i
        if drv is None:
            return None
        rel = path[best:]
        if drv == 0:
            if rel == "":
                return (0, "", (1, 0))   # 内核里根目录 vnode 的 size 恒 0
            r = tree_resolve(rel)
            if r:
                return (0, rel, (r[0], r[1]))
            return None
        if drv == 2:
            # ext2：目录 size 用 i_size（stat 语义跟内核 ext2_open 抄的 VN_SIZE 一致）
            ks = _e2_kindsize(rel)
            if ks:
                return (2, rel, ks)
            return None
        name = rel.lstrip("/")
        if name == "":
            return (1, "", (1, len(devnames)))
        if name in devnames:
            size = len(console_text) if name == "console" else 0
            return (1, name, (2, size))
        return None

    # ---- 命令行的行为，从 kernel.asm 里读回来 ----
    msgs = db_strings(asm)
    prompt = msgs["msg_prompt"]
    cols, rows = 800 // cell_w, 600 // cell_h
    top_row = eq_value(asm, "CONSOLE_TOP_ROW")

    lo = parse_bytes(asm, "kbd_map_lo")
    hi = parse_bytes(asm, "kbd_map_hi")
    assert len(lo) == 128 and len(hi) == 128, (len(lo), len(hi))

    stream = parse_bytes(asm, "selftest_data")
    stream = stream[:eq_value(asm, "selftest_len")]

    # ---- 把扫描码解回字符 ----
    shift, ext, typed = False, False, []
    for sc in stream:
        if sc == 0xE0:
            ext = True
            continue
        code = sc & 0x7F
        if sc & 0x80:
            if code in (0x2A, 0x36):
                shift = False
            ext = False
            continue
        if code in (0x2A, 0x36):
            shift = True
            ext = False
            continue
        if ext:
            ext = False
            continue
        table = hi if shift else lo
        ch = table[code]
        if ch:
            typed.append(chr(ch))
        ext = False
    print("从 kernel.asm 的扫描码流解出的输入：%r" % "".join(typed))

    # ---- 按命令行主循环的规则把这台会话重放一遍 ----
    screen = [[" "] * cols for _ in range(rows)]
    # ping / net 的输出里有往返时间、计数器这类每次都变的数字，
    # 这些行没法逐像素预测，标出来，对账时跳过（但行数必须对，
    # 否则下面的排版就全错了）。
    var = [False] * rows
    varying = [False]
    scrolled = [0]                      # 滚屏次数，横幅要跟着一起上移
    state = {"x": 0, "y": top_row}

    def newline():
        state["x"] = 0
        state["y"] += 1
        if state["y"] >= rows:
            screen.pop(0)
            screen.append([" "] * cols)
            var.pop(0)                  # var 必须跟着一起滚，否则标记会错位
            var.append(False)
            scrolled[0] += 1
            state["y"] = rows - 1

    def putc(ch):
        if ch == "\r":
            newline()
        elif ch == "\b":
            if state["x"] > 0:
                state["x"] -= 1
                screen[state["y"]][state["x"]] = " "
        elif first <= ord(ch) <= first + count - 1:
            screen[state["y"]][state["x"]] = ch
            if varying[0]:
                var[state["y"]] = True
            state["x"] += 1
            if state["x"] >= cols:
                newline()

    def puts(s):
        for ch in s:
            putc(ch)

    def execute(line):
        if not line:
            return
        if line == "help":
            puts(msgs["msg_help"]); newline()
        elif line == "ver":
            puts(msgs["msg_ver"]); newline()
        elif line == "cls":
            for r in screen:
                for i in range(cols):
                    r[i] = " "
            state["x"] = state["y"] = 0
        elif line == "net":
            varying[0] = True
            puts("ip      255.255.255.255"); newline()
            puts("gateway 255.255.255.255"); newline()
            puts("dns     255.255.255.255"); newline()
            puts("netmask 255.255.255.255"); newline()
            puts("mac     FF:FF:FF:FF:FF:FF"); newline()
            puts("link    down  tx/rx 000/000"); newline()
            varying[0] = False
        elif line.startswith("ping"):
            varying[0] = True
            puts("resolving x ... 255.255.255.255"); newline()
            puts("Pinging 255.255.255.255"); newline()
            for _ in range(4):
                puts("Reply from 255.255.255.255: time=999ms"); newline()
            puts("packets: sent=4, received=4"); newline()
            varying[0] = False
        elif line.startswith("echo") and (len(line) == 4 or line[4] == " "):
            puts(line[5:] if len(line) > 4 else ""); newline()
        elif line == "pwd":
            # 自检流里没有 cd，cwd 恒为根目录
            puts("/"); newline()
        elif line == "mount":
            for i, mnt in enumerate(mnts):
                puts(fsnames[i] + " on " + mnt); newline()
        elif line == "ls /dev" or line == "ls /dev/":
            for name in devnames:
                puts(name); newline()
        elif line == "ls /hd" or line == "ls /hd/":
            # ext2：按磁盘条目顺序回放（e2root 的插入序就是目录块遍历序），
            # "." 和 ".." 内核那边吞掉了，这里也不列。
            for name, v in e2root.items():
                puts(name + ("/" if isinstance(v, dict) else "")); newline()
        elif line.startswith("stat ") and len(line) > 5:
            r = vfs_resolve(line[5:])
            if r and r[2][0] < len(type_names):
                tn = type_names[r[2][0]]
            else:
                tn = "unknown"
            if r:
                puts(tn + " size=" + str(r[2][1]) + " fs=" + fsnames[r[0]])
                newline()
            else:
                puts(msgs["msg_unknown"] + line); newline()
        elif line == "ls":
            for name in sorted(root_dir):
                kind = root_dir[name][0]
                puts(name + ("/" if kind == "dir" else "")); newline()
        elif line.startswith("touch "):
            par = tree_parent(line[6:])
            if par and par[1] not in par[0]:
                par[0][par[1]] = ("file", b"")
        elif line.startswith("mkdir "):
            par = tree_parent(line[6:])
            if par and par[1] not in par[0]:
                par[0][par[1]] = ("dir", {})
        elif line.startswith("rm "):
            par = tree_parent(line[3:])
            if par and par[1] in par[0]:
                del par[0][par[1]]
        elif line.startswith("cp ") and len(line.split()) == 3:
            _, src, dst = line.split()
            if dst == "/hd" or dst.startswith("/hd/"):
                # FS4：cp 目标在 /hd → ext2 驱动（截断复用：同名覆盖、条目位置
                # 不动 = dict 重赋值保持插入序，两边语义恰好一致）
                r = tree_resolve(src)
                node = e2root
                parts = [p for p in dst[3:].split("/") if p]
                ok = bool(parts) and r is not None and r[0] == 0
                for p in parts[:-1]:
                    node = node.get(p) if isinstance(node, dict) else None
                    if not isinstance(node, dict):
                        ok = False
                        break
                if ok:
                    node[parts[-1]] = r[2]
            else:
                r = tree_resolve(src)
                par = tree_parent(dst)
                if r and par:
                    if r[0] == 0:
                        par[0][par[1]] = ("file", r[2])
                    else:
                        par[0][par[1]] = ("dir", r[2] if r[2] is not None else {})
        elif line.startswith("mv ") and len(line.split()) == 3:
            _, src, dst = line.split()
            sp = tree_parent(src)
            dp = tree_parent(dst)
            if sp and sp[1] in sp[0] and dp:
                dp[0][dp[1]] = sp[0].pop(sp[1])
        elif line.startswith("cat ") and len(line) > 4:
            path = line[4:]
            if path == "/dev" or path.startswith("/dev/"):
                # /dev 下的 cat 走 devfs 语义（空设备读到 EOF 无输出）
                r = vfs_resolve(path)
                if r and r[2][0] == 2:
                    for ch in console_text if r[1] == "console" else "":
                        if ch == "\n":
                            newline()
                        else:
                            putc(ch)
                else:
                    puts(msgs["msg_unknown"] + line); newline()
            else:
                r = tree_resolve(path)
                if r and r[0] == 0:
                    text = r[2].decode("ascii")
                    for ch in text:
                        if ch == "\n":
                            newline()
                        else:
                            putc(ch)
                    if text and not text.endswith("\n"):
                        newline()
                elif (path == "/hd" or path.startswith("/hd/")) \
                        and isinstance(e2_lookup(path[3:]), bytes):
                    # ext2 文件：内容从磁盘镜像来，末行补换行的语义和 MeFS 分支一致
                    text = e2_lookup(path[3:]).decode("ascii")
                    for ch in text:
                        if ch == "\n":
                            newline()
                        else:
                            putc(ch)
                    if text and not text.endswith("\n"):
                        newline()
                else:
                    puts(msgs["msg_unknown"] + line); newline()
        elif line == "disk":
            # ata.inc 的 cmd_do_disk。型号串来自 IDENTIFY，是虚拟化环境的属性，
            # 仓库里读不出来，只能照 VMware Workstation 的惯例抄一行，并把它
            # 标成可变行（内容不比对，只占一行的排版）；换虚拟化环境要跟着改。
            varying[0] = True
            puts("ata0: VMware Virtual IDE Hard Drive"); newline()
            varying[0] = False
            # 容量和魔数有真值：盘的产物文件本身就是地面真值，从它读，别另抄。
            flat = os.path.join(ROOT, "build", "hd-flat.vmdk")
            img = os.path.join(ROOT, "build", "hd-ext2.img")
            secs = os.path.getsize(flat) // 512
            with open(img, "rb") as fp:
                fp.seek(1024 + 0x38)
                mag = fp.read(2)
            puts("sectors: %d (%d MB)" % (secs, secs // 2048)); newline()
            note = "ext2 magic ok" if mag == b"\x53\xEF" else "no ext2 magic"
            puts("lba2+0x38: %02X %02X (%s)" % (mag[0], mag[1], note)); newline()
        else:
            puts(msgs["msg_unknown"] + line); newline()

    puts(prompt)
    buf = ""
    for ch in typed:
        if ch == "\r":
            newline()
            execute(buf)
            buf = ""
            puts(prompt)
        elif ch == "\b":
            if buf:
                buf = buf[:-1]
                putc("\b")
        else:
            buf += ch
            putc(ch)

    drawn = [("".join(r).rstrip()) for r in screen]
    print("")
    print("重放出来的屏幕：")
    for i, ln in enumerate(drawn):
        if ln:
            print("  %2d | %s" % (i, ln))

    # 横幅的像素会落在文字行 0 和 1 里（横幅从 y=BANNER_Y 开始，每行高 cell_h），
    # 所以逐行对账的时候得把它算进去。
    glyph_h = eq_value(font_inc, "FONT_GLYPH_H")
    glyph_row_bytes = eq_value(font_inc, "FONT_ROW_BYTES")
    font_glyph_len = eq_value(font_inc, "FONT_GLYPH_LEN")
    font_char_num = eq_value(font_inc, "FONT_CHAR_NUM")
    banner_y = eq_value(asm, "BANNER_Y")
    # 横幅是直接画在显存里的，控制台每滚一行，它就整体上移一个文字行的高度。
    # 滚出屏幕顶部的那些扫描线就不该再算了。
    banner_rows = [0] * rows
    shift = scrolled[0] * cell_h
    for g in range(font_char_num):
        base = g * font_glyph_len
        for r in range(glyph_h):
            y = banner_y + r - shift
            if y < 0:
                continue
            band = y // cell_h
            if band >= rows:
                continue
            seg = font_raw[base + r * glyph_row_bytes:base + (r + 1) * glyph_row_bytes]
            banner_rows[band] += sum(bin(b).count("1") for b in seg)

    row_expected = [sum(bits[c] for c in row if c != " ") for row in screen]
    for i in range(rows):
        row_expected[i] += banner_rows[i]

    expected = sum(row_expected)

    # ---- 和内核数出来的对比 ----
    path = find_vmem()
    with open(path, "rb") as fp:
        fp.seek(DIAG_ADDR + D_CONPIX)
        actual = struct.unpack("<I", fp.read(4))[0]
        fp.seek(DIAG_ADDR + D_ROWPIX)
        row_actual = struct.unpack("<%dI" % rows, fp.read(4 * rows))

    print("")
    print("逐行对账（行: 期望 / 实际 / 差）：")
    bad = 0
    skipped = 0
    for i in range(rows):
        if row_expected[i] or row_actual[i]:
            if var[i]:
                skipped += 1
                print("  %2d:   (含可变数字，跳过)" % i)
                continue
            d = row_actual[i] - row_expected[i]
            flag = "" if d == 0 else "   <-- 差 %d" % d
            if d:
                bad += 1
            print("  %2d: %5d / %5d%s" % (i, row_expected[i], row_actual[i], flag))
    print("  逐行核对：%s%s" % ("全部对上" if bad == 0 else "%d 行对不上" % bad,
                            "（跳过 %d 行含可变数字的）" % skipped if skipped else ""))
    # 总数只算能预测的那些行
    expected = sum(v for k, v in enumerate(row_expected) if not var[k])
    actual = sum(v for k, v in enumerate(row_actual) if not var[k])
    print("")
    print("横幅字模置位数 = %d" % banner_bits)
    print("宿主机重算期望 = %d" % expected)
    print("内核数出来的   = %d" % actual)
    diff = actual - expected
    if diff == 0:
        print("结果：完全一致，显存里一个像素都不差")
    elif diff == cell_w * 2:
        print("结果：只差 %d 个像素 = 一条光标横杠（截图时正好闪到亮）" % diff)
    else:
        print("结果：对不上，差 %d 个像素" % diff)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())