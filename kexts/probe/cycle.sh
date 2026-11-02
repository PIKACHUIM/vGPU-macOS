#!/bin/sh
# Rebuild the probe with an experiment selected, then hot-swap it in.
#
#   sh cycle.sh                 -> mode 0, read-only
#   sh cycle.sh 1 1920 1080     -> write WIDTH/HEIGHT
#   sh cycle.sh 2 1920 1080     -> write MAX_WIDTH/MAX_HEIGHT
#
# Unlike the framebuffer kext, the probe owns no hardware and is not retained by
# WindowServer, so `kmutil unload` succeeds and a reboot is not needed between runs.
# The install.sh path (`kmutil load -z`) is kept for cold-boot reproducibility.

set -eu

MODE=${1:-0}
W=${2:-0}
H=${3:-0}
HERE=$(cd "$(dirname "$0")" && pwd)

sh "$HERE/buildx.sh" "$MODE" "$W" "$H" >/dev/null

BUNDLE="$HERE/build/vgpuProbe.kext"
DEST=/Library/Extensions/vgpuProbe.kext

# Unload first: replacing files under a loaded kext leaves the running copy stale, and
# the log would then describe the previous experiment.
kmutil unload -p "$DEST" >/dev/null 2>&1 || true

rm -rf "$DEST"
cp -R "$BUNDLE" "$DEST"
chown -R 0:0 "$DEST"
chmod -R 755 "$DEST"

echo "--- loaded: mode=$MODE ${W}x${H} ---"
kmutil load -z -p "$DEST" 2>&1 | head -5 || true
sleep 1
kextstat 2>/dev/null | grep -i vgpu || echo "(not loaded)"
