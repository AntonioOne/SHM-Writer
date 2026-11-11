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

## Overlay variant (headless)

`ShmFrameTaggerOverlay` is built from the same source (`-DTAG_OVERLAY`). Differences:

- **No window.** It prints attach/detach events and one summary line every 10 s; stop with Ctrl+C.
- **200 Hz** by default, counter **+1 per write** (`--rate` to change).
- Also draws the counter's last four digits (`0000`–`9999`) in the image centre (100×50 area):
  white digits with a black outline, no surrounding shape. The producer's pixels in that area are
  saved after each new VTD frame, and every write composes *saved background + new digits*, so
  digits of the previous value are erased (no ghosting, no visible box).
- If VTD is part-way through copying the centre area, the digits are deferred to the next cycle
  so that a half-written frame is never saved as background. The first-4-pixel counter is
  written every cycle regardless, so frames never repeat.
- Only 8-bit-per-channel formats (RGB8, RGBA8, 8-bit gray) get digits; others only get the counter.
- Optional territory guard between writes: `--poll-us <us>` (0 = spin, one CPU core busy). Off by
  default.
- Links only `opencv_core` and `opencv_imgproc` (no GUI libraries).

Measured with a simulated VTD of 2 × 3840×2160 + 9 × 2560×1440 RGB8 streams (whole-image copy per
frame): the tagger at 200 Hz used about 6 % of one CPU core (the simulated VTD itself 53 % at 30 Hz,
~120 % at 60 Hz).

Run only one of the two programs at a time: both write the first 4 pixels.

## Build

Requires OpenCV 4 (core, imgproc; highgui only for the windowed `ShmFrameTagger`).

```bash
cmake -S . -B build
cmake --build build
```

## Run

```bash
./build/ShmFrameTagger                      # all keys 0x8100-0x81ff at 60 Hz
./build/ShmFrameTagger 0x08130 0x08131      # only these keys
./build/ShmFrameTagger --rate 60 --range 0x8100 0x81ff
./build/ShmFrameTaggerOverlay               # headless, 200 Hz, plus centre digits
./build/ShmFrameTaggerOverlay --rate 300    # other rate
```

Keys: `q`/`ESC` quit, `p` pause/resume writing.

Note: the first pixels in memory are the bottom-left corner of the picture (OpenGL read-back is
bottom-up).
