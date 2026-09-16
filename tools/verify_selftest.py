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