#!/usr/bin/env python3
"""Extract the HD60 USB-bridge firmware from Elgato Game Capture HD for macOS.

The firmware is Elgato's, so it is not distributed with this project. Install
Game Capture HD 2.11.14 (`brew install --cask elgato-game-capture-hd`) and run:

    tools/extract-firmware.py [path/to/BeddoLibrary.dylib] [out.bin]

BeddoLibrary.dylib embeds it as Intel-HEX-like records in a fixed 22-byte
layout: len, pad, addr (u16 LE), type, data[16], checksum. The output file holds
the records in the wire format the device expects on bulk EP 0x01:
len, addr (u16 BE), type, data[len], checksum.
"""

import hashlib
import sys

DEFAULT_DYLIB = "/Applications/Game Capture HD.app/Contents/Frameworks/BeddoLibrary.dylib"
# First record: extended linear address 0x7000 (load base 0x70000000).
FIRST = bytes([0x02, 0x00, 0x00, 0x00, 0x04, 0x70, 0x00])
# sha256 of the wire-format output for Game Capture HD 2.11.14.
KNOWN_SHA256 = "285bae6c7313fe12be513ca6c7a1c6790eb0ff8071a8e306170987004d941ebb"


def extract(blob: bytes) -> bytes:
    start = blob.find(FIRST)
    if start < 0:
        sys.exit("firmware records not found; is this BeddoLibrary.dylib from Game Capture HD 2.11.x?")
    out = bytearray()
    off = start
    while True:
        rec = blob[off:off + 22]
        n, addr, typ, data, cksum = rec[0], rec[2:4][::-1], rec[4], rec[5:5 + rec[0]], rec[21]
        if n > 16 or rec[1] != 0:
            sys.exit(f"unexpected record layout at offset {off}")
        wire = bytes([n]) + addr + bytes([typ]) + data + bytes([cksum])
        if sum(wire) & 0xFF:
            sys.exit(f"bad checksum at offset {off}")
        out += wire
        off += 22
        if typ == 0x01:  # EOF
            return bytes(out)


def main() -> None:
    src = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DYLIB
    dst = sys.argv[2] if len(sys.argv) > 2 else "firmware/hd60-bridge.bin"
    fw = extract(open(src, "rb").read())
    digest = hashlib.sha256(fw).hexdigest()
    if KNOWN_SHA256 and digest != KNOWN_SHA256:
        print(f"warning: sha256 {digest} differs from the tested firmware", file=sys.stderr)
    open(dst, "wb").write(fw)
    print(f"wrote {dst}: {len(fw)} bytes, sha256 {digest}")


if __name__ == "__main__":
    main()
