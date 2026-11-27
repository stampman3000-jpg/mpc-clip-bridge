#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Cross-compile mpc_clip_bridge.so and mpc-clip-ctl for Gen 1 MPC (armv7/armhf, glibc <= 2.31)
# using the cached arm32v7/gcc:11-bullseye Docker image.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$here/build"

docker run --rm --platform linux/arm/v7 \
  -v "$here:/work" -w /work \
  arm32v7/gcc:11-bullseye bash -euo pipefail -c '
    CFLAGS="-O2 -Wall -Wextra -Werror -std=gnu11 -marm -march=armv7-a -mfpu=neon -mfloat-abi=hard -fPIC -fvisibility=hidden"
    gcc $CFLAGS -shared -o build/mpc_clip_bridge.so \
        src/bridge.c src/profiles.c src/proto.c src/gate.S \
        -lpthread -ldl -Wl,--no-as-needed -Wl,-soname,mpc_clip_bridge.so
    gcc -O2 -Wall -Wextra -Werror -std=gnu11 -marm -march=armv7-a -mfpu=neon -mfloat-abi=hard \
        -o build/mpc-clip-ctl src/cli.c

    echo "=== ELF header check ==="
    readelf -h build/mpc_clip_bridge.so | grep -E "Class:|Data:|Machine:|Type:"
    echo "=== Exported dynamic symbols (should be empty; constructor runs via .init_array) ==="
    nm -D --defined-only build/mpc_clip_bridge.so
    echo "=== Max GLIBC version required (must be <= 2.31) ==="
    readelf -V build/mpc_clip_bridge.so | grep -oE "GLIBC_[0-9]+\.[0-9]+(\.[0-9]+)?" | sort -Vu | tail -n 5
    readelf -V build/mpc-clip-ctl | grep -oE "GLIBC_[0-9]+\.[0-9]+(\.[0-9]+)?" | sort -Vu | tail -n 5
'

echo "Built: $here/build/mpc_clip_bridge.so and $here/build/mpc-clip-ctl"
