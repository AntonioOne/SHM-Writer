# ShmFrameTagger

A third writer on VTD RDB image shared memory that guarantees consecutive reads of an image are
never identical — even when VTD stalls.

- Monitors all System V shared memory keys `0x8100`–`0x81ff` (every 0.5 s via `/proc/sysvipc/shm`),
  attaches to those holding RDB images and follows them when they disappear or are recreated.
- Draws a four-digit counter `0000`–`9999` (built-in 5×7 pixel font, white with a 1-pixel black
  outline, 25×9 pixels) in the **top-right corner of the displayed image** of every buffer.
  The counter increases by one per write cycle (`--rate`, default 200 Hz).
- Only the 25×9 stamp area is written, as *saved VTD background + digits*: old digits never remain
  and no box is visible. SHM headers, buffer infos and flags (LOCK / TC / IG) are never touched,
  and VTD is never waited for.
- Between cycles the area is checked every `--poll-us` (default 50 µs). Bytes VTD wrote over it are
  merged into the saved background; once the area is stable again the digits are redrawn. This
  works for any copy order (forward, backward, block-wise, glibc head/tail-last).
- No GUI, no OpenCV; links libstdc++ statically, so the binary needs only glibc ≥ 2.17.

## Layout

```
src/rdb_shm.h     RDB structures (subset of VTD's viRDBIcd.h)
src/digits.*      5x7 font and the 25x9 counter stamp
src/segment.*     one shared memory segment: image lookup, background merge, stamp drawing
src/main.cpp      options, segment discovery, timing loop, status output
package.sh        builds the customer package (dist/)
packaging/        customer README (Chinese)
```

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Run

```bash
./build/ShmFrameTagger                    # keys 0x8100-0x81ff, 200 Hz, check every 50 us
./build/ShmFrameTagger --rate 400         # 400 write cycles per second
./build/ShmFrameTagger 0x08130 0x08131    # only these keys
./build/ShmFrameTagger --poll-us 100      # less CPU, digits restored a bit later after VTD writes
```

Options: `--rate <Hz>`, `--poll-us <us>` (0 = spin, -1 = off), `--range <lo> <hi>`,
`--stats <s>` (status line interval, 0 = off). Stop with Ctrl+C.

## Notes

- The image is stored bottom-up (OpenGL read-back), so the stamp is written into the last 9 rows in
  memory, flipped, so that it reads upright in a viewer that flips the image.
- Supported pixel formats: 8 bits per channel (RGB8, RGBA8, 8-bit gray). Others are reported in the
  status output and left untouched.
- If VTD copies front to back, the stamp area (end of memory) is overwritten right before the frame
  is published, so the digits reappear ~0.1 ms after publishing; with backward copies they are
  usually back before publishing.

## Measurements

Simulated VTD: 2 × 3840×2160 + 9 × 2560×1440 RGB8 streams at 60 Hz, `--rate 400`:

| VTD copy order | digits present at publish | digits seen by a random-time reader | tagger CPU |
|---|---|---|---|
| glibc `memcpy` | 99.4–99.7 % | 100 % | ~10 % of one core |
| backward       | 99.7–100 %  | 100 % | ~13 % |
| forward        | < 1 %       | 98.3 % | ~12.5 % |

VTD stalled at 16 Hz, 30 Hz reader: 0 identical consecutive images and 0 reads without digits
(without the tagger, 109 of 150 reads were identical).

## Packaging

```bash
./package.sh 2.0     # -> dist/ShmFrameTagger-2.0-linux-x86_64.tar.gz (+ .sha256)
```
