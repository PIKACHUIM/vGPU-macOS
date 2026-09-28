#!/bin/sh
# Install the probe kext into /Library/Extensions and stage it for loading.
#
# Run as root on the guest, after build.sh has produced build/vgpuProbe.kext.
#
#   sh build.sh && sudo sh install.sh
#
# Why /Library/Extensions and not /tmp: a staged extension records the path it came
# from and kernelmanagerd re-checks that path on every boot. A bundle under /tmp is
# wiped by the next reboot, which produces
#   Installation check: missing bundle for com.vgpu.probe at /private/tmp/...
# and the kext then silently never loads -- even though it was "approved".
#
# Why `-z` / --no-authorization: without it kmutil refuses with
#   KMErrorDomain Code=27 "Extension with identifiers com.vgpu.probe not approved to load"
# because there is no GUI session to raise the approval prompt. With it the load is
# recorded in /var/db/SystemPolicyConfiguration/KextPolicy and takes effect at once,
# and the following reboot brings it up from the auxiliary kernel collection.
#
# `kmutil install --update-all` and `kmutil rebuild` are NOT usable on this guest:
# both demand a Kernel Debug Kit matching the running build
#   "Missing Developer Kit: As of macOS 13.0, you will need to install a KDK
#    matching your build 24G419 to rebuild kernel collections"
# and `kmutil rebuild` additionally asks for approval through LocalAuthentication,
# which a headless VM cannot answer ("Biometry is not available on this device").
# `kmutil load -z` is the path that works without either.

set -eu

NAME=${NAME:-vgpuProbe}
HERE=$(cd "$(dirname "$0")" && pwd)
BUNDLE="$HERE/build/$NAME.kext"
DEST="/Library/Extensions/$NAME.kext"

if [ ! -d "$BUNDLE" ]; then
    echo "no bundle at $BUNDLE -- run build.sh first" >&2
    exit 1
fi

rm -rf "$DEST"
cp -R "$BUNDLE" "$DEST"
chown -R 0:0 "$DEST"
chmod -R 755 "$DEST"
echo "installed $DEST"

# Drop anything staged from an earlier location, or the stale path wins the next boot.
kmutil clear-staging >/dev/null 2>&1 || true

kmutil load -z -p "$DEST"

echo "--- verify ---"
kmutil showloaded | grep -i "$NAME" || echo "(not in showloaded yet; try a reboot)"
ioreg -rc "$NAME" | head -5
