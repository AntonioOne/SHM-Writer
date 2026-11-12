#!/bin/bash
# Builds the customer package: dist/ShmFrameTagger-<version>-linux-x86_64.tar.gz (+ .sha256).
# The binary links libstdc++/libgcc statically and needs only glibc on the target.
set -euo pipefail
cd "$(dirname "$0")"
VERSION=${1:-2.0}
NAME=ShmFrameTagger-$VERSION-linux-x86_64
OUT=dist/$NAME

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DSTATIC_RUNTIME=ON > /dev/null
cmake --build build-release > /dev/null

rm -rf "$OUT" "dist/$NAME.tar.gz" "dist/$NAME.tar.gz.sha256"
mkdir -p "$OUT"
cp build-release/ShmFrameTagger "$OUT/"
strip --strip-unneeded "$OUT/ShmFrameTagger"
chmod 755 "$OUT/ShmFrameTagger"
cp packaging/README_zh.md "$OUT/README.md"

# Only glibc may remain as a runtime dependency.
if ldd "$OUT/ShmFrameTagger" | grep -v -E "linux-vdso|libc\.so|ld-linux" | grep -q .; then
    echo "ERROR: unexpected runtime dependency:" >&2
    ldd "$OUT/ShmFrameTagger" >&2
    exit 1
fi

tar -C dist -czf "dist/$NAME.tar.gz" "$NAME"
(cd dist && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256")
echo "glibc required: $(objdump -T "$OUT/ShmFrameTagger" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
ls -la dist
