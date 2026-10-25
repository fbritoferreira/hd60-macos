#!/bin/sh
# Experiment: change one encoder parameter (a983 write #N in src/init_seq.h), stream a few
# seconds with a scratch build and print the resulting GOP pattern and stream info.
#   tools/try-param.sh <write-index> <value32>
set -e
cd "$(dirname "$0")/.."
tmp=${TMPDIR:-/tmp}/hd60-try; mkdir -p "$tmp"
python3 -I - "$1" "$2" src/init_seq.h "$tmp/init_seq.h" <<'PY'
import re, sys
idx, val, src, dst = int(sys.argv[1]), int(sys.argv[2], 0), sys.argv[3], sys.argv[4]
lines = open(src).read().split("\n")
n = -1
for i, l in enumerate(lines):
    if "0xa9, 0x83" in l:
        n += 1
        if n == idx:
            b = [int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", l.split("{", 2)[2])]
            lo, hi = val & 0xFFFF, val >> 16
            b[6:8] = [(lo & 0xFF) ^ 0xC7, (lo >> 8) ^ 0x3A]
            b[4:6] = [(hi & 0xFF) ^ 0xE1, (hi >> 8) ^ 0xFE]
            lines[i] = "\t{OP_I2C_W, 0x66, 8, {" + ", ".join(f"0x{x:02x}" for x in b) + "}},"
            print("patched:", lines[i].strip(), file=sys.stderr)
open(dst, "w").write("\n".join(lines))
PY
cp src/hd60.h src/device.h src/device.c src/hd60.c "$tmp/"
cc -O2 -w $(pkg-config --cflags libusb-1.0) -o "$tmp/hd60" "$tmp/hd60.c" "$tmp/device.c" $(pkg-config --libs libusb-1.0)
"$tmp/hd60" -f firmware/hd60-bridge.bin > "$tmp/out.ts" 2>"$tmp/err" & pid=$!
sleep "${SECONDS_TO_RUN:-7}"; kill -INT $pid; wait $pid 2>/dev/null || true
echo "bytes: $(stat -f %z "$tmp/out.ts")"
ffprobe -v error -select_streams v -show_entries stream=width,height,r_frame_rate -of csv=p=0 "$tmp/out.ts" | head -1
ffprobe -v error -select_streams v -show_entries frame=pict_type -of csv=p=0 "$tmp/out.ts" 2>/dev/null | tr -d '\n,' | tail -c 40; echo
