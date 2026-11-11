# ShmFrameTagger

A third writer on VTD RDB image shared memory that guarantees no two reads of the image are
identical frames.

- Monitors all System V shared memory keys `0x8100`–`0x81ff` (every 0.5 s via `/proc/sysvipc/shm`).
- At 60 Hz, writes only the bytes of the **first 4 pixels** of the image data in **every buffer**
  (4 × bytes-per-pixel, e.g. 12 bytes for RGB8). These bytes hold one **big-endian** unsigned
  integer (last byte least significant) that starts at 0 on program start and increases by one
  per write cycle, e.g. `00 … 00 04 D2` = 1234.
- Does not coordinate with other writers (VTD): every buffer is stamped every cycle. If VTD writes
  a new frame over the stamp, that frame is new anyway and the next cycle stamps it again.
- Never writes SHM headers, buffer infos or flags (LOCK / TC / IG).
- Standalone: no VTD headers or libraries required (see `rdb_shm.h`).

The window shows, per segment and buffer: flags, VTD frame number, the value and bytes just written,
the read-back check, write rate and counters.

## Overlay variant

`ShmFrameTaggerOverlay` is built from the same source (`-DTAG_OVERLAY`) and does everything above,
plus draws a 100×50 px ellipse (white fill, red border) in the centre of the image with the counter's
last four digits (`0000`–`9999`), updated every write cycle. Only pixels inside the ellipse are
written. It is drawn upright for viewers that flip the image (OpenGL bottom-up rows) and requires an
8-bit-per-channel format (RGB8, RGBA8, 8-bit gray); for other formats only the first 4 pixels are
written.

Between 60 Hz cycles the overlay build treats the stamp and the ellipse as **owned territory**: it
checks them continuously and rewrites them the moment another writer (VTD) overwrites them. The check
looks at the stamp and the first/last ellipse rows, so a front-to-back image copy is caught while it is
still in progress and the ellipse is restored before the copy finishes.

- `--poll-us 0` (default): spin; uses about one CPU core. With a reader that reads the instant a new
  VTD frame appears, the ellipse was present in 100 % of frames (simulated VTD) and in 600/600 random
  samples on a real VTD.
- `--poll-us 100`: check every 100 µs; about 20 % of one core, ellipse present at the instant of a new
  frame in about 80 % of frames.

The window shows how often the territory was reclaimed (≈ the rate at which VTD writes new frames).

Run only one of the two programs at a time: both write the first 4 pixels.

## Build

Requires OpenCV 4 (core, imgproc, highgui).

```bash
cmake -S . -B build
cmake --build build
```

## Run

```bash
./build/ShmFrameTagger                      # all keys 0x8100-0x81ff at 60 Hz
./build/ShmFrameTagger 0x08130 0x08131      # only these keys
./build/ShmFrameTagger --rate 60 --range 0x8100 0x81ff
./build/ShmFrameTaggerOverlay               # same options, plus the centre ellipse counter
./build/ShmFrameTaggerOverlay --poll-us 100 # cheaper territory check
```

Keys: `q`/`ESC` quit, `p` pause/resume writing.

Note: the first pixels in memory are the bottom-left corner of the picture (OpenGL read-back is
bottom-up).
