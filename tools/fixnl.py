#!/usr/bin/env python3
"""Repair C string literals that were split across a real newline.

Editing these sources from the Windows side put literal newlines where a
backslash-n was intended, which is never valid C. This folds any
double-quoted run that ends at a line break back into an escaped newline, and
normalises the file to LF while it is there.

Usage: python3 tools/fixnl.py FILE [FILE...]
"""
import re
import sys

# A quote, then anything that is neither a quote nor a newline nor a
# backslash-escape, then a newline, then the closing quote on the next line.
SPLIT = re.compile(r'"((?:[^"\\\n]|\\.)*)\n"')


def fix(path):
    with open(path, encoding="utf-8", newline="") as f:
        text = f.read().replace("\r\n", "\n")

    fixed, n = SPLIT.subn(lambda m: '"' + m.group(1) + '\\n"', text)

    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(fixed)
    print("%s: %d repaired" % (path, n))


if __name__ == "__main__":
    for p in sys.argv[1:]:
        fix(p)
