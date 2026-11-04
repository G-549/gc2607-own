#!/bin/bash
# Load v4l2loopback and run the pipeline in the foreground.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${OUT:-/dev/video60}"

if ! lsmod | grep -q '^v4l2loopback'; then
    modprobe v4l2loopback video_nr=60 card_label="GC2607 Own" exclusive_caps=1 \
        || { echo "v4l2loopback not installed (build it from source first)" >&2; exit 1; }
fi

exec "$ROOT/src/gc2607d" -o "$OUT" "$@"
