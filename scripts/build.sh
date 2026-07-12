#!/usr/bin/env bash
# Cross-compile Mark for the Ableton Move (aarch64 Linux) via Docker,
# mirroring schwung's own toolchain (debian:bookworm + gcc-aarch64-linux-gnu)
# so the .so links against the same glibc as the rest of the ecosystem.
#
# Outputs:
#   build/modules/overtake/mark/dsp.so + module.json + ui.js + help.json
#   build/mark-module.tar.gz
set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE=smack-build   # same toolchain image the smack builds use
CFLAGS="-O3 -g -shared -fPIC -Wall -Wextra -Iinclude"

if ! docker image inspect "$IMAGE" &>/dev/null; then
    docker build -t "$IMAGE" - <<'EOF'
FROM debian:bookworm
RUN apt-get update && apt-get install -y gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu file && rm -rf /var/lib/apt/lists/*
EOF
fi

mkdir -p build/modules/overtake/mark
cp modules/overtake/mark/module.json build/modules/overtake/mark/
cp src/ui_overtake.js build/modules/overtake/mark/ui.js
cp src/help_mark.json build/modules/overtake/mark/help.json

# Compile AND tar inside the container: macOS bsdtar embeds AppleDouble
# (._*) xattr entries that Linux tar extracts as real files — the schwung
# installer then reads entries[0] = "._mark" and fails. GNU tar in the
# container produces clean archives.
docker run --rm -v "$PWD":/w -w /w "$IMAGE" bash -c "
    set -e
    aarch64-linux-gnu-gcc $CFLAGS src/mark_core.c src/mark_gen.c \
        -o build/modules/overtake/mark/dsp.so -lm
    file build/modules/overtake/mark/dsp.so
    tar --owner=0 --group=0 -czf build/mark-module.tar.gz -C build/modules/overtake mark
    echo 'tarball contents:'
    tar -tzf build/mark-module.tar.gz
"

echo "Built: build/mark-module.tar.gz"
