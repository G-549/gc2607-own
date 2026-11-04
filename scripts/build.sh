#!/bin/bash
# Build everything from source: userspace daemon + kernel modules.
# No network access, no package installs.
set -euo pipefail
export LC_ALL=C
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KDIR="${KDIR:-/lib/modules/$(uname -r)/build}"

echo "== toolchain"
"$ROOT/scripts/versions.sh"
echo

echo "== userspace daemon"
make -C "$ROOT/src"

if [[ ! -d "$KDIR" ]]; then
    echo
    echo "!! no kernel headers at $KDIR -- skipped the modules."
    echo "   install them (Arch: linux-lts-headers) or pass KDIR=<path>."
    exit 0
fi

echo
echo "== gc2607 sensor module"
make -C "$ROOT/kernel" KDIR="$KDIR"

echo
echo "== ipu-bridge (kernel module, needs a patched source tree)"
echo "see docs/BUILD.md; patch: kernel/0001-ipu-bridge-add-gc2607.patch"
