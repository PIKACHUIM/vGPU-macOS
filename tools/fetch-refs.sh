#!/bin/sh
# Fetch upstream references into refs/. Run from the repository root.
#
# refs/ is git-ignored on purpose: these trees are large and would otherwise
# bloat history. Re-create them with this script instead of committing them.
set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
REFS_DIR="$REPO_ROOT/refs"
DEPTH=${DEPTH:-1}

mkdir -p "$REFS_DIR"

fetch() {
    name=$1
    url=$2
    dest="$REFS_DIR/$name"
    if [ -d "$dest/.git" ]; then
        printf 'update  %s\n' "$name"
        git -C "$dest" fetch --depth "$DEPTH" origin
        git -C "$dest" reset --hard FETCH_HEAD
    else
        printf 'clone   %s\n' "$name"
        git clone --depth "$DEPTH" --quiet "$url" "$dest"
    fi
    printf '        %s @ %s\n' "$name" "$(git -C "$dest" rev-parse --short HEAD)"
}

fetch reims-vgpu        https://github.com/steelbrain/reims-vgpu.git
fetch metal2vulkan      https://github.com/steelbrain/metal2vulkan.git
fetch MacHyperVSupport  https://github.com/acidanthera/MacHyperVSupport.git

printf '\nDone. Mesa svga/vmwgfx are not cloned here; read them at:\n'
printf '  https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/gallium/drivers/svga\n'
