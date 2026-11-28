# mpc-clip-bridge

`mpc-clip-bridge` is an opt-in `LD_PRELOAD` addin that gives MPC plugins a socket API (`/run/mpc-ext.sock`) to the MPC app's clips.

It exposes native multi-track clip read, surgical write, track metadata, and loop-length control without moving the MPC's UI selection. **TrackBoy** is the first client plugin; other plugins and the bundled `mpc-clip-ctl` CLI can use the same socket.

> **WARNING — EXPERIMENTAL / USE AT YOUR OWN RISK.** This is an unofficial addin for **MPC OS `3.10.1.12`**. It hooks internal functions inside `/usr/bin/MPC` via Developer Mode SSH. **Back up your projects and SD card** before installing. Not affiliated with or supported by Akai Professional / inMusic.

## Demo

[![TrackBoy reading and writing MPC clips via mpc-clip-bridge](https://img.youtube.com/vi/pJdCz3IE0R4/hqdefault.jpg)](https://youtu.be/pJdCz3IE0R4)

40-second clip showing TrackBoy reading and writing MPC clips through the bridge.

## Safety

The addin is designed to stay inert unless the running MPC binary is one it knows:

- It arms **only** if `/proc/self/exe` is `/usr/bin/MPC`, the CPU architecture and exact 20-byte GNU Build ID match a per-build profile in `src/profiles.c`, **and** all 3 hook prologues match that profile (8 bytes on ARM32, 16 bytes on AArch64). Those checks run before any code page is modified.
- On any other firmware build, or if any prologue mismatches, the library **refuses to arm and stays completely inert**.
- Kill switches (either one is enough):
  - environment variable `MPC_CLIP_BRIDGE_DISABLE=1`
  - the file `/data/mpc-addins/clip-bridge/disabled`
- This repository contains **no firmware bytes**, MPC app binaries, dumps, or signing salts. Hook addresses and short prologue signatures in the profile table are the only binary-derived data here. See [docs/PORTING.md](docs/PORTING.md) for how new builds are added.

A one-command uninstaller is included (see [Revert / Uninstall](#revert--uninstall)).

## Supported builds

| Profile | Arch | Devices | GNU Build ID | Status |
|---|---|---|---|---|
| `gen1-3.10.1.12` | `armhf` (ELF32 ARM) | Gen 1 MPC One / Live / X / Key 61 | `851d050eb03ce0b8663aaa572b2599813cbf53d4` | Verified on Gen 1 MPC One |
| `gen2-3.10.1.12` | `aarch64` (ELF64 AArch64) | Gen 2 MPC One G2 / XL / Live III / Key 37 G2 | `8b28179d89277a5f73c30be90740766e6166b837` | **Untested on hardware — Gen 2 testers wanted** |

The **original Force** is not a supported target. Additional Build IDs (for example Gen 2 `3.10.1.49`) can be added as data entries in `src/profiles.c` — see [docs/PORTING.md](docs/PORTING.md).

## Protocol

The socket speaks a newline-delimited line protocol on `/run/mpc-ext.sock`. `HELLO` currently advertises **protocol v2** (`proto=2`). The full command and response set is documented in [docs/PROTOCOL.md](docs/PROTOCOL.md).

**Important for VST2 plugins:** never perform synchronous socket I/O on the MPC's UI or audio callbacks (that would deadlock the UI thread). Run socket I/O on a background thread.

The server accepts up to 8 concurrent clients, so TrackBoy and `mpc-clip-ctl` can run at the same time.

## Build & test

Host unit tests and Docker cross-builds (outputs go to `build/`, which is gitignored):

```sh
# 1. Host unit tests (protocol parser/formatter + profile lookup & prologue checks)
cc -std=gnu11 -O2 -Wall -Wextra -Werror -o /tmp/test_proto test/test_proto.c src/proto.c && /tmp/test_proto
cc -std=gnu11 -O2 -Wall -Wextra -Werror -o /tmp/test_profiles test/test_profiles.c src/profiles.c && /tmp/test_profiles

# 2. Cross-compile Gen 1 armhf (glibc <= 2.31, 0 exported symbols) via Docker -> build/
./build_armhf.sh

# 3. Cross-compile Gen 2 aarch64 (glibc <= 2.39, 0 exported symbols) via Docker -> build/aarch64/
./build_aarch64.sh
```

## Install

Release zips follow the [open-mpc](https://github.com/open-mpc/mpc-vst-plugins) catalogue addin layout (`MPC-Clip-Bridge-<ver>-mpc-armv7.zip` for Gen 1, `MPC-Clip-Bridge-<ver>-mpc-aarch64.zip` for Gen 2) and use the canonical `open-mpc` addin installer (`install.sh`, `uninstall.sh`, `addin-lib.sh`; see [docs/INSTALL-DIFF.md](docs/INSTALL-DIFF.md)).

Requires Developer Mode SSH on the MPC:

1. Unzip the release for your device and copy the folder to `/tmp/` on the MPC:
   ```sh
   scp -r MPC-Clip-Bridge-1.0.0 root@<mpc-ip>:/tmp/
   ```
   *(If installing directly from a local build instead of a release zip, copy `build/mpc_clip_bridge.so` + `build/mpc-clip-ctl` (or `build/aarch64/...` on Gen 2) alongside `packaging/addin.manifest`, `packaging/install.sh`, `packaging/uninstall.sh`, and `packaging/addin-lib.sh`.)*
2. Install on the MPC (chains into the shared `90-mpc-addins.conf` drop-in and offers to restart `acvs`):
   ```sh
   ssh -t root@<mpc-ip> "sh /tmp/MPC-Clip-Bridge-1.0.0/install.sh"
   ```
3. Verify live:
   ```sh
   ssh root@<mpc-ip> "/data/mpc-addins/clip-bridge/mpc-clip-ctl hello"
   ssh root@<mpc-ip> "/data/mpc-addins/clip-bridge/mpc-clip-ctl tracks"
   ssh root@<mpc-ip> "/data/mpc-addins/clip-bridge/mpc-clip-ctl list"
   ssh root@<mpc-ip> "/data/mpc-addins/clip-bridge/mpc-clip-ctl set-len 1 0 1920"   # set Track 2 Main loop to 8 steps
   ```

`install.sh` accepts `-y` to restart without asking, or `-n` to skip the restart.

## Revert / Uninstall

```sh
ssh -t root@<mpc-ip> "sh /data/mpc-addins/clip-bridge/uninstall.sh"
```

Emergency kill switch without uninstalling:

```sh
ssh root@<mpc-ip> "touch /data/mpc-addins/clip-bridge/disabled && systemctl restart acvs"
```

## How it works

- Verifies the process, Build ID, and hook prologues as described in [Safety](#safety), then installs 3 hooks: `MpcClipLanes` constructor/destructor (to capture the live root: primary `flag=0` clip lanes and live `Project`) and `PerThreadQueues::dispatch` (UI thread only, verified via `CommandFactory -> ThreadQueues` UI sub-queue at reentrancy depth 0) to run queued socket requests on the MPC's UI thread.
- Reads notes via `MpcClipLanes::Lookup` plus `lower_bound` and libstdc++ `_Rb_tree_increment`.
- Writes, replaces, deletes, and resizes through MPC's own native `CommandFactory` (`ModifySequenceEventListCommand`, `SetClipLengthCommand`, `SetAndEnableIndependentTrackLengthCommand`), so Undo/Redo, Grid View redraw, and `.xpj` saving are preserved on any track or clip — selected or not, Drum, Plugin, Keygroup, MIDI, or CV.

## Credits

- ARM32 trampoline gate and native MPC `ModifySequenceEventListCommand` / `MpcClipLanes` ABI adapted from [zsoltf/mpc-3-ce](https://github.com/zsoltf/mpc-3-ce) (MIT).
- Addin packaging and `LD_PRELOAD` coexistence conventions from [open-mpc/mpc-vst-plugins](https://github.com/open-mpc/mpc-vst-plugins) (`docs/ADDINS.md`).

## Licence

mpc-clip-bridge is released under the MIT License. See [LICENSE](LICENSE).
