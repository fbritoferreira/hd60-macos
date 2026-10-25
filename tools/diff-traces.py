#!/usr/bin/env python3
"""Diff the I2C write sequences of two usbtrace logs (polls and read values ignored).

    tools/diff-traces.py traces/var/base/trace.log traces/var/q1/trace.log
"""
import difflib
import re
import sys

LINE = re.compile(r"bmRT=(..) bReq=(..) wValue=(....) wIndex=(....) wLength=(\d+) done=\d+ kr=(\S+) (IN|OUT) ([0-9a-f]*)")


def writes(path):
    out = []
    for line in open(path):
        if re.search(r"done pipe=3 kr=0x0 n=[1-9]", line):
            break
        m = LINE.search(line)
        if not m or m.group(6) != "0x0":
            continue
        rq, data = m.group(2), m.group(8)
        if rq == "13":
            out.append(f"W {data[2:4]} {data[8:]}")
        elif rq not in ("06", "12"):
            out.append(f"C {rq} {m.group(3)} {data}")
    # Drop register polling noise: consecutive duplicates.
    return [w for i, w in enumerate(out) if i == 0 or w != out[i - 1]]


a, b = writes(sys.argv[1]), writes(sys.argv[2])
print(f"# {len(a)} vs {len(b)} writes")
for line in difflib.unified_diff(a, b, lineterm="", n=int(sys.argv[3]) if len(sys.argv) > 3 else 2):
    print(line)
