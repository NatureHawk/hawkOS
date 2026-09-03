#!/usr/bin/env python3
"""Apply a list of (file, old, new) literal replacements.

Edits are driven from a JSON file so the replacement text travels as data and
never has to survive several layers of shell quoting on the way in. Writes
back with LF endings regardless of the host that produced the JSON.
"""
import json
import sys


def main():
    spec = json.load(open(sys.argv[1], encoding="utf-8"))
    for edit in spec:
        path = edit["file"]
        with open(path, encoding="utf-8", newline="") as f:
            text = f.read().replace("\r\n", "\n")

        old = edit["old"]
        new = edit["new"]
        count = text.count(old)
        if count == 0:
            print("MISS %s: %r" % (path, old[:60]))
            continue
        text = text.replace(old, new)

        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("ok   %s (%d)" % (path, count))


if __name__ == "__main__":
    main()
