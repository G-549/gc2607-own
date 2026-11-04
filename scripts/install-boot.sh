#!/bin/bash
# Install modules and the systemd unit. Run as root.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KVER="$(uname -r)"
DEST="/lib/modules/$KVER/updates"

[[ $EUID -eq 0 ]] || { echo "run as root" >&2; exit 1; }

# 1. sensor module
mkdir -p "$DEST"
install -m644 "$ROOT/kernel/gc2607.ko" "$DEST/"
depmod -a
echo gc2607 > /etc/modules-load.d/gc2607.conf

# 2. virtual camera
install -m644 "$ROOT/v4l2loopback.ko" "$DEST/v4l2loopback.ko" 2>/dev/null || \
    echo "note: build v4l2loopback first and drop v4l2loopback.ko here"
depmod -a
printf 'options v4l2loopback video_nr=60 card_label="GC2607 Own" exclusive_caps=1\n' \
    > /etc/modprobe.d/gc2607-v4l2loopback.conf
echo v4l2loopback > /etc/modules-load.d/v4l2loopback.conf

# 3. daemon + unit
install -m755 "$ROOT/src/gc2607d" /usr/local/bin/gc2607d
install -m644 "$ROOT/systemd/gc2607-camera.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now gc2607-camera.service
echo "done: systemctl status gc2607-camera"
