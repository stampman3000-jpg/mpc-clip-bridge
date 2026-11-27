#!/bin/sh
# SPDX-License-Identifier: MIT
# Install mpc_clip_bridge.so + mpc-clip-ctl onto a Gen 1 MPC (3.10.1.12)
# into /data/mpc-addins/clip-bridge/ and chain LD_PRELOAD cleanly via
# open-mpc's shared 90-mpc-addins.conf drop-in convention.
#
# Run ON the MPC as root:
#   sh install.sh            # install + ask before restarting MPC
#   sh install.sh -y         # install + restart MPC without asking
#   sh install.sh -n         # install without restarting MPC
set -eu

here=$(cd "$(dirname "$0")" && pwd)
dest="/data/mpc-addins/clip-bridge"
so="$dest/mpc_clip_bridge.so"
auto=0
norestart=0
for arg in "$@"; do
    case "$arg" in
        -y) auto=1 ;;
        -n) norestart=1 ;;
    esac
done

[ -f "$here/mpc_clip_bridge.so" ] || { echo "install.sh: mpc_clip_bridge.so missing" >&2; exit 1; }
[ -f "$here/mpc-clip-ctl" ] || { echo "install.sh: mpc-clip-ctl missing" >&2; exit 1; }

# Verify ELF shared object matches host architecture before touching LD_PRELOAD
# (uses od -b + awk for BusyBox compatibility; ARM32 e_machine=40=050, AArch64 e_machine=183=267)
arch=$(uname -m)
magic=$(dd if="$here/mpc_clip_bridge.so" bs=1 count=20 2>/dev/null | od -b | awk '{for(i=2;i<=NF;i++) printf "%s", $i}')
case "$arch:$magic" in
    armv7*:177105114106001001001*003000050000) ;;
    aarch64:177105114106002001001*003000267000) ;;
    *) echo "install.sh: mpc_clip_bridge.so does not match host arch ($arch)" >&2; exit 1 ;;
esac

svc="acvs"
systemctl cat inmusic-mpc >/dev/null 2>&1 && ! systemctl cat acvs >/dev/null 2>&1 && svc="inmusic-mpc"

mkdir -p "$dest"
cp "$here/mpc_clip_bridge.so" "$so.new" && chmod 755 "$so.new" && mv "$so.new" "$so"
cp "$here/mpc-clip-ctl" "$dest/mpc-clip-ctl.new" && chmod 755 "$dest/mpc-clip-ctl.new" && mv "$dest/mpc-clip-ctl.new" "$dest/mpc-clip-ctl"
cp "$here/uninstall.sh" "$dest/uninstall.sh" && chmod 755 "$dest/uninstall.sh"
cp "$here/addin.manifest" "$dest/addin.manifest" && chmod 644 "$dest/addin.manifest"

# Resolve persistent systemd drop-in root.
# On MPC OS 3.10.1, /etc is a late-mounted overlayfs (etc.mount) that appears
# AFTER PID 1 has already loaded acvs.service, so drop-ins under /etc or
# /data/system/etc/overlay are ignored on cold boot. Placing 90-mpc-addins.conf
# under /usr/lib/systemd/system (on rootfs) guarantees systemd loads it on boot.
drop_root=""
remounted_ro=0
cleanup_rootfs() {
    if [ "$remounted_ro" = 1 ]; then
        sync
        mount -o remount,ro / 2>/dev/null || true
        remounted_ro=0
    fi
}
trap cleanup_rootfs EXIT

etc_is_overlay=0
if grep -qE ' /etc overlay ' /proc/mounts 2>/dev/null; then
    etc_is_overlay=1
fi

if [ "$etc_is_overlay" = 1 ]; then
    if touch /usr/lib/systemd/system/.cb_probe 2>/dev/null && rm -f /usr/lib/systemd/system/.cb_probe; then
        drop_root="/usr/lib/systemd/system"
    elif mount -o remount,rw / 2>/dev/null && touch /usr/lib/systemd/system/.cb_probe 2>/dev/null && rm -f /usr/lib/systemd/system/.cb_probe; then
        drop_root="/usr/lib/systemd/system"
        remounted_ro=1
    fi
fi

if [ -z "$drop_root" ]; then
    if touch /etc/systemd/system/.cb_probe 2>/dev/null && rm -f /etc/systemd/system/.cb_probe; then
        drop_root="/etc/systemd/system"
    elif [ -d /data/system/etc/overlay ] && touch /data/system/etc/overlay/.cb_probe 2>/dev/null && rm -f /data/system/etc/overlay/.cb_probe; then
        drop_root="/data/system/etc/overlay/systemd/system"
    else
        drop_root="/run/systemd/system"
        echo "Note: /etc and rootfs are read-only; using /run (non-persistent across reboot)."
    fi
fi

drop_dir="$drop_root/$svc.service.d"
drop_file="$drop_dir/90-mpc-addins.conf"
mkdir -p "$drop_dir"

if [ -f "$drop_file" ]; then
    if ! grep -qF "$so" "$drop_file"; then
        cp "$drop_file" "$drop_file.bak-clip-bridge" 2>/dev/null || true
        if grep -qE '^Environment="LD_PRELOAD=.*"$' "$drop_file"; then
            sed -i "s|^Environment=\"LD_PRELOAD=\(.*\)\"$|Environment=\"LD_PRELOAD=\1:$so\"|" "$drop_file"
        elif grep -qE '^Environment=LD_PRELOAD=' "$drop_file"; then
            sed -i "s|^Environment=LD_PRELOAD=\(.*\)$|Environment=LD_PRELOAD=\1:$so|" "$drop_file"
        else
            printf 'Environment=LD_PRELOAD=%s\n' "$so" >> "$drop_file"
        fi
    fi
else
    cat > "$drop_file" <<EOF
[Service]
Environment=LD_PRELOAD=$so
EOF
fi

cleanup_rootfs

# If we installed to /usr/lib/systemd/system, remove any stale duplicate from
# the late /etc overlay so it cannot shadow the rootfs drop-in after etc.mount.
if [ "$drop_root" = "/usr/lib/systemd/system" ]; then
    stale="/data/system/etc/overlay/systemd/system/$svc.service.d/90-mpc-addins.conf"
    if [ -f "$stale" ] && grep -qF "$so" "$stale"; then
        sed -i "s|:$so||g; s|$so:||g; s|$so||g" "$stale"
        if grep -qE '^Environment=("?LD_PRELOAD="?)?$' "$stale"; then
            rm -f "$stale"
            rmdir "/data/system/etc/overlay/systemd/system/$svc.service.d" 2>/dev/null || true
        fi
        sync
        echo 2 > /proc/sys/vm/drop_caches 2>/dev/null || true
    fi
elif [ "$drop_root" = "/data/system/etc/overlay/systemd/system" ]; then
    sync
    echo 2 > /proc/sys/vm/drop_caches 2>/dev/null || true
fi

systemctl daemon-reload
sync
echo "Installed clip-bridge to $dest (drop-in: $drop_file)."

if [ "$norestart" = 0 ]; then
    ok="n"
    if [ "$auto" = 1 ]; then
        ok="y"
    else
        printf "Restart %s now? Unsaved project changes will be lost. [y/N] " "$svc"
        read -r ok || ok="n"
    fi
    case "$ok" in
        y|Y|yes)
            systemctl restart "$svc"
            echo "Restarted $svc. Verify with: $dest/mpc-clip-ctl hello"
            ;;
        *)
            echo "Restart $svc when ready: systemctl restart $svc"
            ;;
    esac
fi
