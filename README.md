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

- **No window.** Prints attach/detach events and one summary line every 10 s; stop with Ctrl+C.
- **200 Hz** by default (`--rate`), counter **+1 per write**.
- Draws the counter's last four digits (`0000`–`9999`) in the image centre: white digits with a
  black outline, no surrounding shape. Only the rows of the digit band are written, as
  *saved producer background + digits*, so old digits never ghost and no box is visible.
- **Digits are kept visible when VTD writes new frames.** Between writes the band is checked every
  `--poll-us` (default **50 µs**). As soon as VTD's copy has passed the whole band (in either copy
  direction), the new pixels become the background and the digits are redrawn — normally before VTD
  publishes the frame number, so a reader sees the digits on the new frame immediately.
- When VTD stalls, nothing overwrites the band, so the digits (and the first-4-pixel counter) keep
  changing at the write rate: consecutive reads are never identical.
- Only 8-bit-per-channel formats (RGB8, RGBA8, 8-bit gray) get digits; others only get the counter.
- Links only `opencv_core` and `opencv_imgproc` (no GUI libraries).

Measured with a simulated VTD of 2 × 3840×2160 + 9 × 2560×1440 RGB8 streams at 60 Hz (forward,
backward and glibc whole-image copies), tagger `--rate 400`:

| `--poll-us` | digits seen by a random-time reader (newest buffer) | tagger CPU (one core) |
|---|---|---|
| 200 | 99.1–99.5 % | 10–15 % |
| 100 | 99.6–99.9 % | 14–16 % |
| 50 (default) | 100 % | 17–19 % |

With VTD stalled at 16 Hz and a 30 Hz reader: 0 identical consecutive images, 0 reads without
digits (without the tagger: 131 of 180 reads were identical).

Rare race: if VTD crosses the band during the few microseconds the band is written, a few band rows
can show the previous frame's background for one frame (a few per thousand frames).

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
