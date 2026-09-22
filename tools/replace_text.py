#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Replace an exact snippet in a file, once.

    python tools/replace_text.py <target> <old-snippet-file> <new-snippet-file>

Exists because the agent harness cannot always pass multi-line patch
arguments through cmd.exe intact; snippets go through files instead,
which keeps quoting and line endings unambiguous.
"""

import io
import sys


def main():
    if len(sys.argv) != 4:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    target, old_path, new_path = sys.argv[1:4]

    with io.open(target, "r", encoding="utf-8", newline="") as fh:
        text = fh.read()
    with io.open(old_path, "r", encoding="utf-8", newline="") as fh:
        old = fh.read()
    with io.open(new_path, "r", encoding="utf-8", newline="") as fh:
        new = fh.read()

    hits = text.count(old)
    if hits == 0:
        print("old snippet not found in " + target, file=sys.stderr)
        return 1
    if hits > 1:
        print("old snippet is ambiguous (%d hits)" % hits, file=sys.stderr)
        return 1

    with io.open(target, "w", encoding="utf-8", newline="") as fh:
        fh.write(text.replace(old, new, 1))
    print("patched " + target)
    return 0


if __name__ == "__main__":
    sys.exit(main())
