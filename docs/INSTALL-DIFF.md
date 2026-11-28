# Installer Diff: Previous Custom Scripts vs. Canonical `open-mpc` Addin Installer

This document compares `mpc-clip-bridge`'s previous custom `packaging/install.sh` and `packaging/uninstall.sh` against the canonical `open-mpc` addin installer (`tools/release/addin/{install.sh,uninstall.sh,addin-lib.sh}` from `open-mpc/mpc-vst-plugins`).

**Decision:** Adopt the canonical `open-mpc` scripts verbatim with no fork. Every problem our custom scripts attempted to solve is handled more safely and completely by the canonical installer, and no custom hook is needed.

---

## Side-by-Side Comparison

### 1. Systemd `LD_PRELOAD` Drop-in & Coexistence
- **Previous custom scripts:**
  - Hardcoded `90-mpc-addins.conf` without preserving an existing base `LD_PRELOAD` list from the service unit or recording `# base:` / `# lib:` metadata.
  - Checked only `acvs` and `inmusic-mpc` (did not handle `ac50`).
  - Did not detect third-party drop-ins sorting after `90-mpc-addins.conf` (e.g. `hwremap.conf`) or launchers that override `LD_PRELOAD` themselves (MockbaMod `AddOns/run_*.sh`, Hakai `/usr/bin/az01-launch-MPC`).
  - `uninstall.sh` checked `! [ -w "$f" ]` before remounting `/` read-write, which is unreliable under BusyBox (`[ -w ]` reports true for `root` even on a read-only mount).
- **Canonical installer:**
  - Detects `ac50`, `acvs`, and `inmusic-mpc`.
  - Uses write-probe files instead of `[ -w ]`.
  - Preserves any firmware/base `LD_PRELOAD` via `# base:` and drop-in format versioning (`# lib: 2`), edits writable overriding units/drop-ins in place with `.bak-mpc-addins` backups, and cleanly coexists with MockbaMod and Hakai launchers.

### 2. `/etc` Overlay Cold-Boot Timing (MPC OS 3.10.1)
- **Previous custom scripts:**
  - Detected `/etc` as an `overlay` mount, remounted `/` read-write, and wrote `90-mpc-addins.conf` directly into `/usr/lib/systemd/system/<svc>.service.d/` so PID 1 would see it before `etc.mount`.
  - Also removed any duplicate entry from `/data/system/etc/overlay/systemd/system/<svc>.service.d/90-mpc-addins.conf` and echoed `2` to `/proc/sys/vm/drop_caches`.
- **Canonical installer:**
  - Keeps `90-mpc-addins.conf` in `/etc` — or, on MPC OS 3.10.1 with Akai's read-only `user-access/etc-overlay` stacked over the writable `/etc` overlay, writes directly to the writable upper layer (`etc_stacked_upper()`) and refreshes the dentry cache (`etc_refresh()` -> `drop_caches=2`).
  - Solves the late `etc.mount` cold-boot race cleanly via `boot_reload_add`: writes `/usr/lib/systemd/system/mpc-addins-reload.service` (linked from `acvs.service.wants/`, `After=etc.mount`, `Before=acvs.service`, `ConditionDirectoryNotEmpty=/etc/systemd/system/acvs.service.d`, `ExecStart=-/bin/systemctl daemon-reload`) and removes it via `boot_reload_remove` when the last addin is uninstalled.
- **Upgrade note for existing installs:** Users who previously ran our old custom `install.sh` (which placed `90-mpc-addins.conf` under `/usr/lib/systemd/system/acvs.service.d/`) must remove that stale rootfs drop-in before installing a `1.0.0+` catalogue release zip. Because the old custom `uninstall.sh` hit BusyBox's `[ -w ]` bug on read-only `/`, remove it directly over SSH first:
  ```sh
  mount -o remount,rw / && rm -f /usr/lib/systemd/system/acvs.service.d/90-mpc-addins.conf && rmdir /usr/lib/systemd/system/acvs.service.d 2>/dev/null || true; sync; mount -o remount,ro /
  ```

### 3. Gen 2 `dm-verity` / Read-Only Rootfs
- **Previous custom scripts:**
  - Tried `mount -o remount,rw /`, then probed `/etc`, `/data/system/etc/overlay`, and finally fell back to `/run/systemd/system` with a non-persistent warning.
  - Did **not** copy `install.sh` or `mpc-plugin.json` into `/data/mpc-addins/clip-bridge/`, so re-arming after a reboot required keeping the release folder around.
- **Canonical installer:**
  - Distinguishes read-only `/etc` overlays (3.10 Live III, no `upperdir`) from stacked overlays (3.10.1 Key 37) and `dm-verity` rootfs (`remount,rw /` refused), falling back cleanly to `/run/systemd/system/<svc>.service.d/90-mpc-addins.conf` with a clear non-persistent warning.
  - Copies `install.sh`, `uninstall.sh`, `addin-lib.sh`, `addin.manifest`, and `mpc-plugin.json` (`SELF_FILES`) into `/data/mpc-addins/clip-bridge/`, allowing USB autorun or one-line re-arming after reboot (`sh /data/mpc-addins/clip-bridge/install.sh -y -n -t /data/mpc-addins/clip-bridge`) without the release zip.

### 4. ELF Architecture & File Staging Checks
- **Previous custom scripts:**
  - Checked ELF magic/class/machine via `dd | od -b | awk` against `uname -m`, staged `.so` and `mpc-clip-ctl` as `.new` -> `mv` with `chmod 755`.
- **Canonical installer:**
  - Checks both `mpc-plugin.json` `arch` against `uname -m` (printing clear Gen 1 vs. Gen 2 guidance on mismatch) and ELF32 ARM / ELF64 AArch64 headers via BusyBox-compatible `od -b | sed` (`check_so`).
  - Stages every file as `.new` -> `mv` and automatically sets `755` on ELF binaries / `#!` scripts (`mpc-clip-ctl`) even after transfer through FAT/exFAT media.

### 5. MPC Restart, Verification, and Uninstall Cleanup
- **Previous custom scripts:**
  - Supported `-y` and `-n`, restarted `acvs` (or `inmusic-mpc`), and printed a verify command without checking whether MPC actually loaded the `.so`.
  - On uninstall, ran `rm -rf /data/mpc-addins/clip-bridge` and `rm -f /run/mpc-ext.sock` **before** restarting MPC.
- **Canonical installer:**
  - Supports `-y`, `-n` (`DEFER=1` for batch installers like `mpc-store.sh` and the desktop app), and `-t <dir>`.
  - After restarting MPC, polls `/proc/<pid>/environ` (`check_loaded`, up to 40s) to verify the `.so` is actually present in MPC's `LD_PRELOAD`, warning immediately if a launcher dropped it.
  - On uninstall, restarts MPC **before** removing installed files, and deletes only files tracked by the manifest (`remove_files`), preserving any extra user files (such as the `/data/mpc-addins/clip-bridge/disabled` kill-switch file) and removing the directory once empty.
  - **Socket file difference (`/run/mpc-ext.sock`):** Our old `uninstall.sh` ran `rm -f /run/mpc-ext.sock`. The canonical `uninstall.sh` does not need to do this because `bridge.c`'s destructor unlinks `/run/mpc-ext.sock` on clean MPC shutdown (and `/run` is `tmpfs` cleared on reboot). Even if a stale socket path survived a hard crash, `mpc_clip_bridge.so` unlinks it before `bind()` and clients receive `ECONNREFUSED` when no server is listening.

---

## Conclusion

Nothing in the previous custom scripts requires keeping a forked `install.sh` or `uninstall.sh` or adding a custom hook. Replacing them with the canonical `open-mpc` scripts (`install.sh`, `uninstall.sh`, `addin-lib.sh`) fixes BusyBox `[ -w ]` edge cases, adds Gen 2 self-reinstall and `mpc-store.sh` / desktop app compatibility, handles MPC OS 3.10.1 stacked overlays and cold-boot reload via `mpc-addins-reload.service`, and passes `catalog_check.py --catalog`.
