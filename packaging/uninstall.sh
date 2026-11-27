#!/bin/sh
# SPDX-License-Identifier: MIT
# Cleanly uninstall mpc_clip_bridge.so without disturbing other open-mpc addins.
set -eu

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

svc="acvs"
systemctl cat inmusic-mpc >/dev/null 2>&1 && ! systemctl cat acvs >/dev/null 2>&1 && svc="inmusic-mpc"

remounted_ro=0
cleanup_rootfs() {
    if [ "$remounted_ro" = 1 ]; then
        sync
        mount -o remount,ro / 2>/dev/null || true
        remounted_ro=0
    fi
}
trap cleanup_rootfs EXIT

for root in /usr/lib/systemd/system /etc/systemd/system /data/system/etc/overlay/systemd/system /run/systemd/system; do
    f="$root/$svc.service.d/90-mpc-addins.conf"
    [ -f "$f" ] || continue
    if grep -qF "$so" "$f"; then
        if [ "$root" = "/usr/lib/systemd/system" ] && ! [ -w "$f" ]; then
            if mount -o remount,rw / 2>/dev/null; then
                remounted_ro=1
            fi
        fi
        sed -i "s|:$so||g; s|$so:||g; s|$so||g" "$f"
        # If LD_PRELOAD is now empty, remove the drop-in
        if grep -qE '^Environment=("?LD_PRELOAD="?)?$' "$f"; then
            rm -f "$f"
            rmdir "$root/$svc.service.d" 2>/dev/null || true
        fi
        if [ "$root" = "/usr/lib/systemd/system" ]; then
            cleanup_rootfs
        elif [ "$root" = "/data/system/etc/overlay/systemd/system" ]; then
            sync
            echo 2 > /proc/sys/vm/drop_caches 2>/dev/null || true
        fi
    fi
done

rm -rf "$dest"
rm -f /run/mpc-ext.sock
systemctl daemon-reload
sync
echo "Removed clip-bridge."

if [ "$norestart" = 0 ]; then
    ok="n"
    if [ "$auto" = 1 ]; then
        ok="y"
    else
        printf "Restart %s now? Unsaved project changes will be lost. [y/N] " "$svc"
        read -r ok || ok="n"
    fi
    case "$ok" in
        y|Y|yes) systemctl restart "$svc" ;;
    esac
fi
