# hd60-macos

A userspace driver for the original **Elgato Game Capture HD60** (USB `0fd9:005c`) on macOS, including Apple Silicon. Elgato never shipped a native driver for this card, and its legacy Intel-only software is on its way out. This driver boots the card, configures it, and streams its MPEG-TS output (H.264 1080p60 plus audio) to OBS or any other player.

It talks to the card through libusb. It needs no kernel extension, no Rosetta, and no Elgato software at runtime.

> Not affiliated with or endorsed by Elgato / Corsair. "Elgato" and "Game Capture HD60" are their trademarks.

## Status

| What | State |
|---|---|
| Cold boot (firmware upload) | Works |
| HDMI input, 1080p60 | Works, tested with a PS5 |
| Stream out to stdout or UDP | Works |
| Other input resolutions | Untested; the init sequence was recorded at 1080p60 |
| HDCP-protected sources | Black picture; turn HDCP off on the console |
| Shows up as a camera in OBS | Not yet; use a Media Source (below) |
| Survives unplug or replug | No; restart `hd60` |

Only the original HD60 is supported. The HD60 S, HD60 S+, HD60 X and HD60 Pro are different hardware, and the S+ and X are already plug-and-play (UVC) on macOS.

## Install

You need Homebrew, the Xcode command line tools, and libusb:

```sh
brew install libusb pkg-config
git clone https://github.com/fbritoferreira/hd60-macos.git
cd hd60-macos
make
```

### Firmware

The card runs a small firmware image that the host uploads at power-on. The image is Elgato's, so this repo doesn't include it. Extract it from Elgato's own app:

```sh
brew install --cask elgato-game-capture-hd    # Game Capture HD 2.11.14, only needed for this step
mkdir -p firmware
python3 tools/extract-firmware.py             # writes firmware/hd60-bridge.bin
```

The extractor checks the result against the SHA-256 of the tested image
(`285bae6c…941ebb`). After that you can uninstall Game Capture HD again.

## Use

Quit Elgato Game Capture HD first, because only one program can hold the card.

```sh
# Watch it directly
./hd60 | ffplay -fflags nobuffer -flags low_delay -framedrop -

# Record to a file
./hd60 > capture.ts

# Stream to OBS over local UDP
./hd60 -u 127.0.0.1:5000
```

Options: `-f <path>` sets the firmware file (default `firmware/hd60-bridge.bin`), `-u ip:port` sends UDP instead of stdout, and `-v` prints progress. Stop it with Ctrl+C.

### OBS

1. Start `./hd60 -u 127.0.0.1:5000`.
2. In OBS, add a **Media Source** with these settings:
   - **Local File**: unchecked
   - **Input**: `udp://127.0.0.1:5000?overrun_nonfatal=1&fifo_size=50000000`
   - **Input Format**: `mpegts`
   - **Network Buffering**: `0 MB`
   - **Use hardware decoding when available**: checked
   - **Restart playback when source becomes active**: unchecked
   - **FFmpeg Options**: `fflags=nobuffer`

Audio is carried in the stream, so the Media Source plays it too.

### Black picture?

If the picture is black, the source is sending HDCP-protected video. On a PS5 go to *Settings › System › HDMI* and turn off *Enable HDCP*. On a PS4 it's *Settings › System › Enable HDCP*.

### Latency

The card encodes in hardware with an `IBBP` GOP (two B-frames, I-frame every 12), which costs a few frames of latency no matter what the host does. The settings above keep OBS from adding more on top. Turning off B-frames would need the encoder's configuration registers to be decoded. Contributions welcome.

## How it works

The HD60 is a USB 2.0 device with one vendor-specific interface:

| Endpoint | Use |
|---|---|
| EP0 | vendor control requests |
| `0x01` OUT bulk | firmware upload |
| `0x82` IN bulk | MPEG-TS stream (H.264 High@4.0 1080p59.94, MPEG-1 Layer II audio) |
| `0x81`, `0x83` IN bulk | not used by this driver |

**Boot.** Until firmware is running, vendor request `0x02` stalls. The host then sends the firmware as Intel-HEX-style binary records on EP `0x01`. Each record is `len, addr_hi, addr_lo, type, data[len], checksum`, one per bulk write, starting with an extended-linear-address record for `0x7000` and ending with the EOF record `00 00 00 01 ff`. The host finishes with the 8-byte command `08 00 00 00 70 00 00 00`, which starts execution at `0x70000000`. After that, `0x02` returns the VID and PID.

**Control.** After boot the card behaves like a USB-to-I²C bridge:

| Request | Direction | Format |
|---|---|---|
| `0x06` | IN, 2 bytes | status; `00 01` means the I²C engine is ready. The host polls it before every I²C op |
| `0x13` | OUT | I²C write: `13, addr, 0xC0\|len, 0x10, payload[len]` |
| `0x12` | IN | I²C read: `wValue = (0xC0\|len) << 8 \| addr`, `wIndex = 0x0010` |
| `0x0c`, `0x0f`, `0x10` | OUT | GPIO and bridge setup during init |

The I²C devices are at 8-bit addresses `0x66`, `0x9c`, `0x98`, `0x64` and `0x02`. The first three are the same devices the [elgato-gchd](https://github.com/tolga9009/elgato-gchd) project reaches through the original Game Capture HD's "mail" ports `0x33`, `0x4e` and `0x4c`, and the payloads match byte for byte. For example, `0x9c` takes `{reg, value ^ 0xcc}`-style pairs identical to gchd's `configure_hdmi.cpp`, and `0x66` takes gchd's 3-byte `99 89 ed` and `99 89 ec` register accesses. gchd's notes on those registers apply here.

**Init.** `src/init_seq.h` is the 2,292-step configuration sequence that Elgato's app performs after boot. `tools/gen-replay.py` generated it from a USB trace. Status polls are folded into ready-waits, and register polls into mask/expect loops. The driver replays it and then reads EP `0x82`.

## Tools

- `tools/usbtrace/usbtrace.c` is a `DYLD_INSERT_LIBRARIES` shim that logs every IOKit USB call an app makes, including control requests, bulk and async transfers, and pipe data. Game Capture HD's entitlements allow injection, so it works without re-signing:

  ```sh
  clang -arch x86_64 -dynamiclib -o usbtrace.dylib tools/usbtrace/usbtrace.c -framework IOKit -framework CoreFoundation
  mkdir -p traces/run && open -n --env DYLD_INSERT_LIBRARIES=$PWD/usbtrace.dylib \
      --env USBTRACE_DIR=$PWD/traces/run -a "Game Capture HD"
  ```

  This writes `trace.log` plus `pipe<N>.bin`, the raw stream data.
- `tools/gen-replay.py` converts a cold-boot `trace.log` into `src/init_seq.h`.
- `tools/extract-firmware.py` extracts the firmware, as described above.

Traces from other resolutions, inputs or settings would help: they're how the init sequence gets turned into real, parameterised configuration. Unplug and replug the card before tracing so the trace starts from a cold boot. Traces contain whatever was on screen, so check them before sharing.

## Roadmap

- Decode the register writes into named settings, covering resolution, bitrate and B-frames, instead of a fixed replay
- A CoreMediaIO camera extension, so the card shows up as a normal camera in OBS, FaceTime and other apps
- Recovering from hot-plug and reconnecting automatically

## Credits

The register-level groundwork comes from [elgato-gchd](https://github.com/tolga9009/elgato-gchd) by Tolga Cakir and contributors (MIT).

## License

MIT. See [LICENSE](LICENSE). Elgato's firmware is not covered and is not distributed here.
