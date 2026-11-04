#!/bin/bash
# Install modules and boot-time configuration. Run as root.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KVER="$(uname -r)"
DEST="/lib/modules/$KVER/updates"

[[ $EUID -eq 0 ]] || { echo "run as root" >&2; exit 1; }
mkdir -p "$DEST"
install -m644 "$ROOT/kernel/gc2607.ko" "$DEST/"
depmod -a

echo gc2607 > /etc/modules-load.d/gc2607.conf
echo "installed; ipu-bridge must already be patched and installed for the sensor to appear"
