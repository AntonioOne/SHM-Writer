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
  merged into the saved background; once no new byte has arrived for 3 µs the digits are redrawn.
  This works for any copy order (forward, backward, block-wise, glibc head/tail-last).
- VTD publishes the frame number ~0.2 ms *before* it copies the pixels. After a new frame number
  the buffer is therefore polled continuously (no sleeping, at most 5 ms) until the stamp is back,
  so it is missing only for microseconds.
- A transiently unparsable message header (VTD rewrites it when publishing) does not interrupt the
  guarding of a buffer.
- `--realtime` runs with SCHED_FIFO priority so the restore is not delayed by the scheduler.
- **Single instance:** a lock file `/tmp/ShmFrameTagger.lock` (flock + PID) ensures only one writer
  runs. Starting it again stops the running instance first (SIGTERM, SIGKILL after 5 s); an
  instance owned by another user is reported instead. `--stop` stops the running instance.
  On exit (Ctrl+C, SIGTERM, SIGHUP, `--stop`) the digits are removed from the images.
- No GUI, no OpenCV; links libstdc++ statically, so the binary needs only glibc ≥ 2.17.

## Layout

```
src/rdb_shm.h     RDB structures (subset of VTD's viRDBIcd.h)
src/digits.*      5x7 font and the 25x9 counter stamp
src/segment.*     one shared memory segment: image lookup, background merge, stamp drawing
src/single_instance.*  lock file: one running instance, --stop
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
sudo setcap cap_sys_nice+ep build/ShmFrameTagger && ./build/ShmFrameTagger --rate 400 --realtime
```

Options: `--rate <Hz>`, `--poll-us <us>` (0 = spin, -1 = off), `--realtime`, `--range <lo> <hi>`,
`--stats <s>` (status line interval, 0 = off). Stop with Ctrl+C or `./build/ShmFrameTagger --stop`.

## Notes

- The image is stored bottom-up (OpenGL read-back), so the stamp is written into the last 9 rows in
  memory, flipped, so that it reads upright in a viewer that flips the image.
- Supported pixel formats: 8 bits per channel (RGB8, RGBA8, 8-bit gray). Others are reported in the
  status output and left untouched.
- If VTD copies front to back, the stamp area (end of memory) is overwritten right before the frame
  is published, so the digits reappear ~0.1 ms after publishing; with backward copies they are
  usually back before publishing.

## Measurements

Real VTD (2 × 960×540 RGB8 streams), `--rate 400`, continuous reads of the newest buffer's stamp
area (~10 million reads per second):

| configuration | reads without digits | stamp missing per VTD frame (median / max) | CPU |
|---|---|---|---|
| before hot polling | 0.2 % | 12 µs / 2.5 ms | 7 % |
| hot polling, normal priority | 0.03 % | 12 µs / 1.8 ms | 7 % |
| hot polling, `--realtime` | **0.003 %** | **5 µs / 13 µs** | 6 % |


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
./package.sh 2.2     # -> dist/ShmFrameTagger-2.2-linux-x86_64.tar.gz (+ .sha256)
```
