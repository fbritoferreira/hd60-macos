#!/usr/bin/env python3
"""Turn a usbtrace cold-boot log into src/init_seq.h, the HD60 init sequence.

    tools/gen-replay.py stop=traces/quit/trace.log 1080p60=traces/cold1/trace.log \
        720p60=traces/var/h720/trace.log 1080p30=traces/var/fps30/trace.log > src/init_seq.h

Rules:
- Firmware upload (bulk EP 0x01) and failed requests (pre-firmware probing) are dropped;
  the driver does those itself.
- req 0x06 status polls are dropped; the driver waits for ready before every I2C op.
- Runs of the same I2C write followed by a read are the app polling a register; they
  become one POLL op that repeats until the last value seen in the trace.
- Everything stops at the first bulk IN data on pipe 3 (video is flowing).
"""

import re
import sys

LINE = re.compile(
    r"bmRT=(..) bReq=(..) wValue=(....) wIndex=(....) wLength=(\d+) done=\d+ kr=(\S+) (IN|OUT) ([0-9a-f]*)"
)


def init_lines(path):
    """Startup section of a trace: up to the first video data after the last encoder parameter
    block (a9 83 writes). Mode changes make the app restart the encoder, so earlier stream starts
    don't count."""
    lines = open(path).read().split("\n")
    last = max(i for i, l in enumerate(lines) if "OUT 1366c810a983" in l)
    end = next(i for i in range(last, len(lines)) if re.search(r"done pipe=3 kr=0x0 n=[1-9]", lines[i]))
    return lines[:end]


def parse(lines):
    ops = []
    for line in lines:
        m = LINE.search(line)
        if not m:
            continue
        rt, rq, wv, wi, wl, kr, _, data = m.groups()
        if kr != "0x0" or rq == "06":
            continue
        rq, wv, wi, wl = int(rq, 16), int(wv, 16), int(wi, 16), int(wl)
        d = bytes.fromhex(data)
        if rq == 0x13:
            ops.append(("W", d[1], d[4:]))
        elif rq == 0x12:
            ops.append(("R", wv & 0xFF, wl, d))
        elif rt == "40":
            ops.append(("OUT", rq, wv, wi, d))
        else:
            ops.append(("IN", rq, wv, wi, wl))
    return ops


# Registers whose semantics are known from elgato-gchd (mail port 0x33 == I2C 0x66).
KNOWN_POLLS = {(0x66, bytes([0x99, 0x89, 0xED])): (0x40, 0x40)}  # pollOn0x9989ED: wait for bit 6
DATA_READS = {(0x66, bytes([0x99, 0x89, 0xEC]))}  # readFrom0x9989EC: data, never a poll


def fold_polls(ops):
    """Fold write+read runs where the value holds steady then changes into one POLL op.

    POLL compares only the bits that changed (mask = first ^ last). Runs whose values
    vary throughout (e.g. 99 89 ec data reads) are kept as plain ops.
    """
    out = []
    i = 0
    while i < len(ops):
        if ops[i][0] == "W" and i + 1 < len(ops) and ops[i + 1][0] == "R":
            w, r = ops[i], ops[i + 1]
            key = (w[1], w[2])
            if key in KNOWN_POLLS:
                j = i
                while j + 1 < len(ops) and ops[j] == w and ops[j + 1][:3] == r[:3]:
                    j += 2
                out.append(("POLL", w[1], w[2], *KNOWN_POLLS[key]))
                i = j
                continue
            if key in DATA_READS:
                out += [w, r]
                i += 2
                continue
            vals = []
            j = i
            while j + 1 < len(ops) and ops[j] == w and ops[j + 1][:3] == r[:3]:
                vals.append(ops[j + 1][3])
                j += 2
            if len(vals) >= 2 and len(set(vals[:-1])) == 1 and vals[-1] != vals[0] and r[2] == 1:
                mask = vals[0][0] ^ vals[-1][0]
                out.append(("POLL", w[1], w[2], mask, vals[-1][0] & mask))
                i = j
                continue
        out.append(ops[i])
        i += 1
    return out


def c_bytes(b):
    return "{" + ", ".join(f"0x{x:02x}" for x in b) + "}"


def stop_lines(path):
    """Shutdown section of a clean-quit trace: from the encoder stop (a4 4f) before the pipe
    abort, up to (not including) the final ab a9 0f a4 55 handshake, which the driver does itself."""
    lines = open(path).read().split("\n")
    abort = next(i for i, l in enumerate(lines) if "AbortPipe pipe=3" in l)
    start = max(i for i in range(abort) if "OUT 1366c210a44f" in lines[i])
    hs = [i for i, l in enumerate(lines) if "OUT 1366c510aba90fa455" in l and i > abort]
    return lines[start:hs[0]]


def emit(name, ops):
    print(f"static const struct hd60_op {name}[] = {{")
    for op in ops:
        k = op[0]
        if k == "W":
            print(f"\t{{OP_I2C_W, 0x{op[1]:02x}, {len(op[2])}, {c_bytes(op[2])}}},")
        elif k == "R":
            print(f"\t{{OP_I2C_R, 0x{op[1]:02x}, {op[2]}, {{0}}}},  // trace: {op[3].hex()}")
        elif k == "POLL":
            # data = write payload; mask/expect on the 1-byte read
            print(f"\t{{OP_I2C_POLL, 0x{op[1]:02x}, {len(op[2])}, {c_bytes(op[2])}, 0x{op[3]:02x}, 0, 0, 0x{op[4]:02x}}},")
        elif k == "OUT":
            print(f"\t{{OP_CTRL_OUT, 0x{op[1]:02x}, {len(op[4])}, {c_bytes(op[4])}, 0, 0x{op[2]:04x}, 0x{op[3]:04x}}},")
        else:
            print(f"\t{{OP_CTRL_IN, 0x{op[1]:02x}, {op[4]}, {{0}}, 0, 0x{op[2]:04x}, 0x{op[3]:04x}}},")
    print("};")
    print(f"// {name}: {len(ops)} ops", file=sys.stderr)


def main():
    """gen-replay.py stop=<clean-quit trace> <mode>=<trace> ...

    Each <mode> becomes hd60_init_<mode>[], the startup sequence for that output mode.
    """
    print("// Generated by tools/gen-replay.py from Game Capture HD traces. Do not edit.")
    print("// hd60_init_<mode>: startup to stream start for each output mode (HDMI input).")
    print("// hd60_stop_seq: encoder stop and HDMI receiver power-down on clean quit.")
    print("#pragma once")
    print('#include "hd60.h"')
    modes = []
    for arg in sys.argv[1:]:
        name, path = arg.split("=", 1)
        if name == "stop":
            emit("hd60_stop_seq", fold_polls(parse(stop_lines(path))))
        else:
            emit(f"hd60_init_{name}", fold_polls(parse(init_lines(path))))
            modes.append(name)
    print("static const struct hd60_mode hd60_modes[] = {")
    for m in modes:
        print(f'\t{{"{m}", hd60_init_{m}, sizeof hd60_init_{m} / sizeof hd60_init_{m}[0]}},')
    print("};")


if __name__ == "__main__":
    main()
