# rodecaster-ndi-hx

[![License: MIT](https://img.shields.io/github/license/VadaLinux76/rodecaster-ndi-hx)](LICENSE)
[![CI](https://github.com/VadaLinux76/rodecaster-ndi-hx/actions/workflows/ci.yml/badge.svg)](https://github.com/VadaLinux76/rodecaster-ndi-hx/actions/workflows/ci.yml)

🇮🇹 Italian idea and project, born from one evening of debugging against a real RØDECaster
Video. Read this README in [🇮🇹 Italiano](README.md) (the more detailed, original version).

Turns a USB webcam plugged into a Raspberry Pi 4 into a real **NDI|HX** source (not standard
NDI/SpeedHQ), using the Pi4's hardware H.264 encoder. Built to feed a
[RØDECaster Video](https://rode.com/en-us/rodecaster/rodecaster-video), which only accepts
certified NDI|HX2+ — but any NDI|HX receiver should work.

```
USB webcam (MJPEG/YUYV) --ffmpeg--> H.264 (bcm2835-codec hardware encoder) --ndi_hx_send--> NDI|HX
```

## Why not one of the many "webcam to NDI" tools already out there

Common "webcam to NDI" tools send *standard* NDI (compressed internally as SpeedHQ by the
library), which is much heavier on bandwidth and **not what an HX-only device expects**.
Real NDI|HX requires sending already-compressed H.264/HEVC packets using the low-level APIs of
the **NDI Advanced SDK** — a capability that isn't very well documented, with a few non-obvious
gotchas (see below). This repo is the result of that debugging session, so whoever runs into
the same wall afterwards doesn't have to start from scratch.

## Requirements

- Raspberry Pi 4 (needs the `bcm2835-codec` hardware H.264 encoder, exposed by ffmpeg as
  `h264_v4l2m2m`) — on other hardware you'll need to adapt `capture_cam_hx.sh`
- A USB webcam that speaks MJPEG or YUYV over V4L2 (`v4l2-ctl --list-formats-ext`)
- `ffmpeg` with `h264_v4l2m2m` support (standard on Raspberry Pi OS / Ubuntu for recent Pi
  models)
- **NDI Advanced SDK** — *not included in this repo* (proprietary to Vizrt/NDI, free to use but
  not redistributable). Download it from
  [ndi.video](https://ndi.video/for-developers/ndi-advanced/) (requires free registration),
  extract it, and pass the path to `build_hx.sh`/`build_inspect.sh` via `NDI_SDK_DIR` if it's
  not in `./ndi-adv-sdk` next to the scripts.

  Note: the freely downloadable version of the SDK is marked "development use" and self-limits
  streams to 30 minutes — for continuous/commercial use you need to contact
  `licensing@ndi.video`. We never hit that limit during the testing described below.

## Build

```bash
./build_hx.sh        # -> ndi_hx_send (the sender)
./build_inspect.sh    # -> ndi_hx_inspect (diagnostic tool, optional)
```

If the SDK has multiple `lib/<arch>` subfolders (a multi-architecture package), the one
matching the host (`uname -m`) is picked automatically; to bypass detection and pick one
explicitly, set `NDI_SDK_LIBDIR=/exact/path`.

Alternatively, [CMake](#build-with-cmake) offers the same targets plus `ctest`/`install`.

## Code architecture

Annex-B stream parsing and access unit reconstruction live in `annexb.h`/`annexb.cpp`, a
module with **no dependency on the NDI SDK** — `ndi_hx_send.cpp` remains a thin wrapper that
uses it and translates each assembled frame into an `NDIlib_compressed_packet_t`. The
separation exists specifically to make the trickiest part (parsing/assembly) testable in CI
without needing the proprietary SDK, which isn't automatically downloadable there.

```bash
# annexb module unit tests (no dependencies, runs anywhere with just g++/clang++)
g++ -std=c++17 -Wall -Wextra -Wpedantic -o test_annexb tests/test_annexb.cpp annexb.cpp
./test_annexb

# same, with AddressSanitizer + UndefinedBehaviorSanitizer (what CI runs)
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -o test_annexb_san tests/test_annexb.cpp annexb.cpp
./test_annexb_san
```

### Build with CMake

An alternative to the bash scripts above, with standard `build`/`test`/`install` targets. The
scripts remain the simplest option for anyone who doesn't want to install CMake.

```bash
# just the annexb module + tests (no SDK required)
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure

# also ndi_hx_send/ndi_hx_inspect, linked against the given SDK
cmake -B build -DNDI_SDK_DIR=/path/to/ndi-adv-sdk && cmake --build build
```

## Usage

```bash
./capture_cam_hx.sh <device> <width> <height> <fps> <ndi_name> [bitrate] [gop_frame] [profile] [level]

# example:
./capture_cam_hx.sh /dev/video0 1920 1080 30 "Pi4 Cam HX" 8M 2

# <fps> also accepts a fraction N/D (e.g. 30000/1001, the classic NTSC "29.97fps") — only if
# your webcam actually supports that exact frame interval, check with
# `v4l2-ctl --list-formats-ext`
./capture_cam_hx.sh /dev/video0 1920 1080 30000/1001 "Pi4 Cam HX" 8M 2
```

To have it start at boot, copy `ndi-webcam-send.service` to `/etc/systemd/system/`
(replacing the `<user>` placeholders and your webcam's path), then:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now ndi-webcam-send.service
```

## Self-diagnosis

```bash
./doctor.sh /dev/video0
```

Checks all prerequisites (ffmpeg, hardware encoder, webcam formats, NDI SDK, compiled
binaries, network discovery) and reports what's missing. Read-only checks only: it never
touches the webcam while streaming, so it's safe to run even while the service is already
active.

## `ndi_hx_inspect`: see what's actually arriving

```bash
./ndi_hx_inspect "<NDI source name>"
```

Connects as an NDI receiver in `COMPRESSED` mode and prints the real structure of the packets
it receives (FourCC, keyframe, sizes, extra_data). Useful both for debugging your own sender
and for inspecting how a real certified HX source (e.g. the RODE Capture app on a smartphone)
behaves, to compare the two.

## The 6 non-obvious bugs (the useful part if you got here from a desperate search)

The starting symptom, on a real certified receiver (RØDECaster Video): NDI connection
established normally, tally working, but video rejected with
`unsupported, resolution 0x0, frame rate 23, format N/A`. A homemade verification tool (that
just reads bytes without actually decoding them) can look like everything works even when the
content is semantically wrong — which is why these bugs survived local testing for so long.

1. **The `*_lowest_bandwidth` FourCC is not "the same video at a lower bitrate"**: per the NDI
   spec it's reserved for a second stream, a *preview at a fixed 640px width*. For the main,
   full-resolution stream you should always use `*_highest_bandwidth`, whatever the actual
   resolution you're sending is. (Initially this looked like an SDK licensing issue, since
   frames tagged `lowest_bandwidth` never reached any receiver — the real cause is that the SDK
   drops them if they don't respect the 640px constraint.)

2. **The real cause of "resolution 0x0 / format N/A"**: video data and extra_data (SPS/PPS)
   must be in **Annex-B** format (start code `00 00 00 01`), *not* AVCC/length-prefixed (the
   MP4/avcC style). The
   [official documentation](https://docs.ndi.video/all/developing-with-ndi/advanced-sdk/using-h.264-h.265-and-aac-codecs/sending-video-frames)
   says so explicitly, but it's easy to miss and assume AVCC out of habit.

3. `NDIlib_video_frame_v2_t.picture_aspect_ratio` **cannot be 0** for compressed streams — it
   needs the real ratio (e.g. `(float)width / height`). With 0, a strict receiver can end up
   reading invalid dimensions.

4. `frame.timecode` must be the packet's **real PTS**, not the
   `NDIlib_send_timecode_synthesize` sentinel.

5. The Pi4's hardware encoder defaults to **H.264 High Profile**. Setting a more compatible
   profile (e.g. Main) with `v4l2-ctl --set-ctrl` *before* starting ffmpeg **has no effect**:
   V4L2 mem2mem devices are stateless, every `open()` (including ffmpeg's) creates an
   independent instance with the hardware defaults. The profile has to be passed to ffmpeg
   itself, as a **numeric value** (`-profile:v 77` for Main, `100` for High — string names like
   `"main"` aren't accepted by `h264_v4l2m2m`).

6. `h264_v4l2m2m` ignores `-g` on its own: you need explicitly forced keyframes with
   `-force_key_frames "expr:eq(mod(n,GOP),0)"`. In this config the GOP is kept very short (one
   keyframe every 2 frames, ~66ms) because the NDI SDK reports at runtime that to be
   "NDI|HX compliant" an I-frame must arrive within 100ms of a receiver connecting.

## Limitations

- **The Pi4 sits at a steady ~95% CPU** (measured at 1920x1080@30) while the stream is active.
  The culprit isn't the H.264 encoder (that's hardware): it's the software decode of the
  incoming MJPEG from the webcam plus the colorspace conversion done by ffmpeg on the CPU,
  which alone keep 2+ of the 4 cores busy. Fine on a Pi4 dedicated only to this, but don't
  expect headroom to do anything else on the same machine.
- The Pi4 has no hardware HEVC encoder (decode only) — so this does NDI|HX2 (H.264), not
  HX3/HEVC. HX3 via software encoding would probably be too heavy for the Pi4 in real time (on
  top of the already-high CPU usage from the point above).
- Tested with a single 1920x1080@30 MJPEG USB webcam and a RØDECaster Video. Other
  webcam/receiver combinations may hold further surprises — issues and PRs welcome.
- The CI (badge above) validates the bash scripts (`shellcheck`) and the `annexb.h`/`.cpp`
  module (build + test on GCC and Clang, with `-Wall -Wextra -Wpedantic -Wconversion -Werror`
  and under AddressSanitizer/UndefinedBehaviorSanitizer). It does **not** compile
  `ndi_hx_send.cpp` / `ndi_hx_inspect.cpp` in full: the NDI Advanced SDK isn't automatically
  downloadable in CI (it requires manual registration on ndi.video), and those two files are
  the only ones that depend on it — also the reason the delicate logic was extracted into
  `annexb`.
- **Multi-slice access units**: `ndi_hx_send` reassembles them correctly (via Access Unit
  Delimiter when present, or by reading `first_mb_in_slice` from the slice header when absent
  — see `annexb.h`). This introduces a one-access-unit latency: a frame is sent only when the
  first slice of the next one arrives, or at end of stream. With the Pi4's hardware encoder
  (`h264_v4l2m2m`, which doesn't slice) that extra latency is therefore negligible.
- The CI (badge above) also verifies the CMake build, but only the part that doesn't depend on
  the NDI SDK (the `annexb` module + tests): `ndi_hx_send`/`ndi_hx_inspect` via CMake require
  `-DNDI_SDK_DIR=...` and aren't therefore compiled automatically in CI, for the same reason as
  the point above.

## Debug

`ndi_hx_send` can write the raw bytes of the first keyframe (SPS/PPS, extra_data, full NDI
packet) to `/tmp/hx_debug_*` for out-of-band inspection with `ffprobe`/`hexdump` — useful if
something doesn't add up with a new encoder/webcam. Off by default: enable it with the
`NDI_HX_DEBUG` environment variable (any value, just needs to be set):

```bash
NDI_HX_DEBUG=1 ./capture_cam_hx.sh /dev/video0 1920 1080 30 "Pi4 Cam HX"
```

## License

This code is released under the MIT license (see `LICENSE`). The NDI Advanced SDK needed to
compile it **is not included** and is subject to Vizrt/NDI's own license
(see [ndi.video](https://ndi.video/for-developers/ndi-advanced/)).

## For AI assistants/agents

Two files meant to be read by an LLM, following the [llms.txt](https://llmstxt.org/)
convention:

- [`llms.txt`](llms.txt) — lightweight version: just an index and links to the relevant files,
  with a one-line description each
- [`llms-full.txt`](llms-full.txt) — full version: the complete content of the README,
  sources, scripts and config consolidated into a single file, no need to follow links or
  clone the repo
