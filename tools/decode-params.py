#!/usr/bin/env python3
"""Decode the encoder parameter block (I2C 0x66, command a9 83) from usbtrace logs.

Each write is a9 83 <addr16> <value32>, scrambled by XOR. The value is little-endian,
low half XOR c7 3a (bytes 6-7), high half XOR e1 fe (bytes 4-5). The address key is
not known yet, so addresses are shown raw.

    tools/decode-params.py traces/var/base/trace.log traces/var/h720/trace.log ...
"""
import re
import sys

W = re.compile(r"bReq=13 .*kr=0x0 OUT 1366c810(a983[0-9a-f]{12})")


def blocks(path):
    """Split the trace's a983 writes into blocks (a new block starts after a gap of other ops)."""
    out, cur, gap = [], [], 0
    for line in open(path):
        m = W.search(line)
        if m:
            x = bytes.fromhex(m.group(1))
            lo = int.from_bytes(bytes(a ^ b for a, b in zip(x[6:8], b"\xc7\x3a")), "little")
            hi = int.from_bytes(bytes(a ^ b for a, b in zip(x[4:6], b"\xe1\xfe")), "little")
            cur.append((x[2:4].hex(), hi << 16 | lo))
            gap = 0
        elif "bReq=13" in line:
            gap += 1
            if gap > 200 and cur:
                out.append(cur)
                cur = []
    if cur:
        out.append(cur)
    return out


def main():
    names = [p.split("/")[-2] for p in sys.argv[1:]]
    last = {n: blocks(p)[-1] for n, p in zip(names, sys.argv[1:])}
    for n, p in zip(names, sys.argv[1:]):
        print(f"# {n}: {len(blocks(p))} blocks, last has {len(last[n])} writes", file=sys.stderr)
    keys = []
    for n in names:
        for a, _ in last[n]:
            if a not in keys:
                keys.append(a)
    print("addr  " + "".join(f"{n:>24}" for n in names))
    for a in keys:
        row = [",".join(str(v) for k, v in last[n] if k == a) or "-" for n in names]
        print(f"{a}  " + "".join(f"{r:>24}" for r in row) + ("  *" if len(set(row)) > 1 else ""))


if __name__ == "__main__":
    main()
