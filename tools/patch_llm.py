#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Wire the LLM/TCP module into kernel.asm.

Idempotent: running it twice changes nothing.

  1. net.inc 的 dispatch 里 .try_net 分支原本直接跳到 .try_echo，
     插进去的 llm 分支落在一句 jmp .done 后面，永远走不到。
     这里补一个 .try_llm 标号并把跳转接过去。
  2. 把 llm.inc 挂到 net.inc 后面一起汇编。
  3. 去掉早先试探时留下的、现在没人用的变量。
"""

import io
import sys

KERNEL = "src/kernel/kernel.asm"

DISPATCH_OLD = """    jz .try_echo
    call cmd_do_net
    jmp .done
    mov esi, line_buf
    mov edi, cmd_llm
"""

DISPATCH_NEW = """    jz .try_llm
    call cmd_do_net
    jmp .done
.try_llm:
    mov esi, line_buf
    mov edi, cmd_llm
"""

INCLUDE_OLD = '%include "net.inc"\n'
INCLUDE_NEW = '%include "net.inc"\n%include "llm.inc"\n'

DROP_PREFIXES = ("tcp_srv_ip", "llm_len", "llm_state")


def main():
    with io.open(KERNEL, "r", encoding="utf-8") as fh:
        text = fh.read()

    changed = False

    if ".try_llm:" in text:
        print("dispatch: already patched")
    elif DISPATCH_OLD in text:
        text = text.replace(DISPATCH_OLD, DISPATCH_NEW, 1)
        changed = True
        print("dispatch: wired .try_llm")
    else:
        print("dispatch: ANCHOR NOT FOUND", file=sys.stderr)
        return 1

    if 'llm.inc' in text:
        print("include: already present")
    elif INCLUDE_OLD in text:
        text = text.replace(INCLUDE_OLD, INCLUDE_NEW, 1)
        changed = True
        print("include: added llm.inc")
    else:
        print("include: ANCHOR NOT FOUND", file=sys.stderr)
        return 1

    kept = []
    dropped = []
    for line in text.split("\n"):
        stripped = line.lstrip()
        if any(stripped.startswith(p + " ") or stripped.startswith(p + "\t")
               for p in DROP_PREFIXES):
            dropped.append(stripped.split()[0])
            changed = True
            continue
        kept.append(line)
    if dropped:
        text = "\n".join(kept)
        print("vars: dropped unused " + ", ".join(dropped))
    else:
        print("vars: nothing to drop")

    if changed:
        with io.open(KERNEL, "w", encoding="utf-8", newline="") as fh:
            fh.write(text)
        print("kernel.asm written")
    else:
        print("kernel.asm unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
