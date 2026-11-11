#!/bin/bash
# Builds the customer package: dist/ShmFrameTaggerOverlay-<version>-linux-x86_64.tar.gz
# The binary uses RPATH=$ORIGIN/lib and ships the OpenCV core/imgproc + TBB libraries it needs.
set -euo pipefail
cd "$(dirname "$0")"
VERSION=${1:-1.0}
NAME=ShmFrameTaggerOverlay-$VERSION-linux-x86_64
OUT=dist/$NAME
LIBDIR=$(pkg-config --variable=libdir opencv4)

rm -rf "$OUT" "dist/$NAME.tar.gz"
mkdir -p "$OUT/lib"
g++ -std=c++14 -O2 -DTAG_OVERLAY -DNDEBUG main.cpp -o "$OUT/ShmFrameTaggerOverlay" \
    $(pkg-config --cflags opencv4) -lopencv_imgproc -lopencv_core -pthread \
    -Wl,-rpath,'$ORIGIN/lib' -Wl,--disable-new-dtags -Wl,--as-needed
for lib in libopencv_core.so.4.2 libopencv_imgproc.so.4.2 libtbb.so.2; do
    cp -L "$(ldconfig -p | awk -v l="$lib" '$1 == l {print $NF; exit}')" "$OUT/lib/"
done
strip --strip-unneeded "$OUT/ShmFrameTaggerOverlay" "$OUT"/lib/*
chmod 755 "$OUT/ShmFrameTaggerOverlay"; chmod 644 "$OUT"/lib/*
cp packaging/README_zh.md "$OUT/README.md"

# Every library must resolve inside the package (except the base system libraries).
if ldd "$OUT/ShmFrameTaggerOverlay" | grep -E "opencv|tbb" | grep -v "$OUT/lib/"; then
    echo "ERROR: a bundled library resolves outside the package" >&2; exit 1
fi
tar -C dist -czf "dist/$NAME.tar.gz" "$NAME"
(cd dist && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256")
ls -la dist
