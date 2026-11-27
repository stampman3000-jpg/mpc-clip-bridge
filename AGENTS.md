# AGENTS.md: mpc-clip-bridge

LD_PRELOAD addin for MPC OS 3.10.1.12 (Gen 1 armhf and Gen 2 aarch64) that serves `/run/mpc-ext.sock`, giving native multi-track Clip Matrix read/write. TrackBoy is the first client plugin. Plus `mpc-clip-ctl` CLI for testing.

- Build: `./build_armhf.sh` (outputs to `build/`, never committed) and `./build_aarch64.sh` (outputs to `build/aarch64/`, never committed). Host tests: `test/test_proto.c`, `test/test_profiles.c`.
- Install/uninstall on MPC (Developer Mode SSH): `packaging/install.sh`, `packaging/uninstall.sh`.
- Kill switch: `MPC_CLIP_BRIDGE_DISABLE=1` or create `/data/mpc-addins/clip-bridge/disabled`.
- Per-build profiles live in `src/profiles.h` / `src/profiles.c` (see `docs/PORTING.md`); the addin refuses to arm unless architecture, GNU Build ID, and all 3 hook prologues match exactly.

Rules:
- Never commit build outputs, binaries, or anything copied from the MPC (`/usr/bin/MPC`, firmware, dumps) or inMusic signing salts.
- Commit as Stampman3000 <stampman3000@gmail.com>.
- Only the machine owner should deploy to a hardware MPC; save the project first.
