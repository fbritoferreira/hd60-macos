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
| Output modes | 1080p60, 720p60 and 1080p30 (scaled on the card), plus bitrate control |
| OBS source plugin | Works; video and audio in-process |
| Clean shutdown, reconnect after unplug | Works |
| HDCP-protected sources | Black picture; turn HDCP off on the console (see below for PS3) |
| Camera extension for other apps | Builds, untested: needs a paid Apple developer team to sign |

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

Options:

| Option | Meaning |
|---|---|
| `-m 1080p60\|720p60\|1080p30` | output mode, scaled by the card's hardware (default 1080p60) |
| `-b <kbps>` | average bitrate, up to 40000 (default about 40000 at 1080p60) |
| `-u ip:port` | send MPEG-TS over UDP instead of stdout |
| `-f <path>` | firmware file (default `firmware/hd60-bridge.bin`) |
| `-v` | print progress |

`hd60` waits for the card if it isn't plugged in and reconnects after an unplug. Stop it with Ctrl+C, which also shuts the card down cleanly.

### OBS plugin (recommended)

The plugin runs the driver inside OBS and decodes with OBS's own FFmpeg, so you get video and audio with no helper process and the lowest latency. It builds against your installed OBS. Tested with OBS 32.2.2, which bundles FFmpeg 8.1:

```sh
brew install simde
mkdir -p obs-plugin/deps && cd obs-plugin/deps
git clone --depth 1 --branch 32.2.2 --filter=blob:none --sparse https://github.com/obsproject/obs-studio.git
git -C obs-studio sparse-checkout set libobs
git clone --depth 1 --branch n8.1 --filter=blob:none --sparse https://github.com/FFmpeg/FFmpeg.git ffmpeg
git -C ffmpeg sparse-checkout set libavcodec libavformat libavutil libswresample
cd ../.. && make install-obs-plugin   # copies the plugin and your firmware into OBS's plugin folder
```

Match the OBS tag to your OBS version, and the FFmpeg tag to the libavcodec major version OBS ships (`otool -L /Applications/OBS.app/Contents/Frameworks/libavcodec.dylib`).

Restart OBS, then go to **Sources → + → Elgato HD60**. The source properties pick the output mode and bitrate. Quit any standalone `hd60` first, because only one program can hold the card.

### OBS without the plugin

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

**PS3:** the PS3 always enables HDCP on HDMI and has no setting to turn it off. This project doesn't and won't bypass HDCP. The PS3 does send games without HDCP over **component video**, though: connect a PS3 component AV cable to a component-to-HDMI converter, and the converter's HDMI output to the HD60. Then on the PS3 choose *Settings › Display Settings › Video Output Settings › Component / D-Terminal*, at 720p or 1080i.

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

**Init.** `src/init_seq.h` holds one startup sequence per output mode, plus a shutdown sequence. Each is about 2,300 steps. `tools/gen-replay.py` generated them from USB traces of Elgato's app: status polls are folded into ready-waits, and register polls into mask/expect loops. The driver replays the chosen sequence, then reads EP `0x82`. On stop it replays the shutdown sequence and runs the processor-off handshake from gchd: write `ab a9 0f a4 55` until the read-back is `33 44 55`.

Status `0x06` isn't a hard gate. Elgato's app waits at most about 100 ms for it and then proceeds, and the driver does the same.

**Encoder parameters.** The H.264 encoder takes 80 writes to I²C `0x66` of the form `a9 83 <addr16> <value32>`. The value is scrambled: bytes 4–5 are the high 16 bits (little-endian) XOR `e1 fe`, and bytes 6–7 are the low 16 bits XOR `c7 3a`. `tools/decode-params.py` decodes a trace. The registers identified so far:

| Register (raw) | Meaning | 1080p60 value |
|---|---|---|
| `597b` | width in 16-px macroblocks | 120 |
| `5f7b` | height in macroblocks | 68 |
| `4d7b` | bottom crop in pixels | 8 |
| `d77a` | GOP length | 12 (6 at 30 fps) |
| `5d7b` | frame-rate divisor | 1001 |
| `f37a` | average bitrate, kbps | 39997 |
| `fd7a` / `d97a` | max / secondary max bitrate, kbps (2× / 1.8× average, capped at 40000) | 40000 |
| `db7a` | min bitrate, kbps (0.7× average) | 27997 |
| `fd7e` | peak bitrate, kbps (2.2× average, capped at 43501) | 43501 |
| `f97e` idx `0b` | 1 = fixed IBBP GOP, 0 = adaptive B-frame placement | 1 |
| `f17a` idx `29` | GOP structure (5 normally; 1 wedges the encoder until a replug) | 5 |

Some registers take several writes, with an index in the top byte of the value. `tools/try-param.sh <n> <value>` patches one write and reports the resulting stream. Careful: a bad value can wedge the encoder until the card is unplugged.

## Tools

- `tools/usbtrace/usbtrace.c` is a `DYLD_INSERT_LIBRARIES` shim that logs every IOKit USB call an app makes, including control requests, bulk and async transfers, and pipe data. Game Capture HD's entitlements allow injection, so it works without re-signing:

  ```sh
  clang -arch x86_64 -dynamiclib -o usbtrace.dylib tools/usbtrace/usbtrace.c -framework IOKit -framework CoreFoundation
  mkdir -p traces/run && open -n --env DYLD_INSERT_LIBRARIES=$PWD/usbtrace.dylib \
      --env USBTRACE_DIR=$PWD/traces/run -a "Game Capture HD"
  ```

  This writes `trace.log` plus `pipe<N>.bin`, the raw stream data.
- `tools/gen-replay.py` converts traces into `src/init_seq.h` (see its docstring).
- `tools/trace-variants.sh` traces Game Capture HD once per encoder setting, and `tools/diff-traces.py` diffs the resulting I²C writes.
- `tools/decode-params.py` decodes the encoder parameter block, and `tools/try-param.sh` experiments with single parameters.
- `tools/extract-firmware.py` extracts the firmware, as described above.

Traces from other resolutions, inputs or settings would help: they're how the init sequence gets turned into real, parameterised configuration. Unplug and replug the card before tracing so the trace starts from a cold boot. Traces contain whatever was on screen, so check them before sharing.

### Camera extension (untested)

`camera/` contains a CoreMediaIO camera extension and host app, built with XcodeGen, that would expose the card as "HD60 Camera" (1080p60, video only) to every app. It builds, but macOS only loads system extensions signed by a **paid** Apple developer team, so it hasn't been run yet. With a paid team:

```sh
cd camera && DEVELOPMENT_TEAM=<team id> xcodegen
xcodebuild -project HD60Camera.xcodeproj -scheme HD60Camera -configuration Release -allowProvisioningUpdates
```

Copy `HD60Camera.app` to `/Applications`, open it and click Install. Reports welcome.

## Roadmap

- Find the B-frame setting (a "no B-frames" mode would cut a few frames of latency)
- 720p30 and other input resolutions (needs traces)
- Test the camera extension with a paid developer team

## Credits

The register-level groundwork comes from [elgato-gchd](https://github.com/tolga9009/elgato-gchd) by Tolga Cakir and contributors (MIT).

## License

MIT. See [LICENSE](LICENSE). Elgato's firmware is not covered and is not distributed here.
