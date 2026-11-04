#!/bin/bash
# Build everything from source: userspace daemon + two kernel modules.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KDIR="${KDIR:-/lib/modules/$(uname -r)/build}"

[[ -d "$KDIR" ]] || { echo "missing kernel headers: $KDIR" >&2; exit 1; }

echo "== userspace daemon"
make -C "$ROOT/src"

echo "== gc2607 sensor module"
make -C "$ROOT/kernel" KDIR="$KDIR"

echo "== ipu-bridge module (patched kernel module)"
echo "apply $ROOT/kernel/0001-ipu-bridge-add-gc2607.patch to a kernel source tree,"
echo "then build drivers/media/pci/intel/ipu-bridge.c against $KDIR."
