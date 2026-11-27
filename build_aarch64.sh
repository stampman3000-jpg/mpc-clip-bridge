#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Cross-compile mpc_clip_bridge.so and mpc-clip-ctl for Gen 2 MPC (aarch64, glibc <= 2.39)
# using the cached arm64v8/gcc:12-bookworm Docker image (Debian 12 / glibc 2.36).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$here/build/aarch64"

docker run --rm --platform linux/arm64 \
  -v "$here:/work" -w /work \
  arm64v8/gcc:12-bookworm bash -euo pipefail -c '
    CFLAGS="-O2 -Wall -Wextra -Werror -std=gnu11 -fPIC -fvisibility=hidden"
    gcc $CFLAGS -shared -o build/aarch64/mpc_clip_bridge.so \
        src/bridge.c src/profiles.c src/proto.c src/gate_aarch64.S \
        -lpthread -ldl -Wl,--no-as-needed -Wl,-soname,mpc_clip_bridge.so
    gcc -O2 -Wall -Wextra -Werror -std=gnu11 \
        -o build/aarch64/mpc-clip-ctl src/cli.c

    echo "=== ELF header check ==="
    readelf -h build/aarch64/mpc_clip_bridge.so | grep -E "Class:|Data:|Machine:|Type:"
    echo "=== Exported dynamic symbols (should be empty; constructor runs via .init_array) ==="
    nm -D --defined-only build/aarch64/mpc_clip_bridge.so
    echo "=== Max GLIBC version required (must be <= 2.39) ==="
    readelf -V build/aarch64/mpc_clip_bridge.so | grep -oE "GLIBC_[0-9]+\.[0-9]+(\.[0-9]+)?" | sort -Vu | tail -n 5
    readelf -V build/aarch64/mpc-clip-ctl | grep -oE "GLIBC_[0-9]+\.[0-9]+(\.[0-9]+)?" | sort -Vu | tail -n 5
'

echo "Built: $here/build/aarch64/mpc_clip_bridge.so and $here/build/aarch64/mpc-clip-ctl"
