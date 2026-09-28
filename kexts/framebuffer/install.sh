#!/bin/sh
# Install vgpuFramebuffer into /Library/Extensions and request it for loading.
#
#   sh build.sh && sudo sh install.sh
#
# READ THIS FIRST: on this guest the kext will NOT load.
#
# A kext that subclasses IOFramebuffer has to be linked into a kernel collection, and every
# route to building one is closed without a KDK (docs/kext-loading.md has the measurements).
# `kmutil load` returns Code=28 "requires a reboot" and the reboot never delivers it. A plain
# IOService kext such as kexts/probe loads fine, so the block is specific to family classes.
#
# To actually run this kext you need one of:
#   A. the KDK for the running build, so that `kmutil create --update-all` can build the
#      auxiliary collection; or
#   B. OpenCore Kernel -> Add, which injects at prelink with no KDK and no collection.
#
# The steps below are still correct and are kept so the flow is ready when A or B lands.

set -eu

NAME=${NAME:-vgpuFramebuffer}
HERE=$(cd "$(dirname "$0")" && pwd)
BUNDLE="$HERE/build/$NAME.kext"
DEST="/Library/Extensions/$NAME.kext"
BUNDLE_ID="com.vgpu.framebuffer"
POLICY="/var/db/SystemPolicyConfiguration/KextPolicy"

if [ ! -d "$BUNDLE" ]; then
    echo "no bundle at $BUNDLE -- run build.sh first" >&2
    exit 1
fi

rm -rf "$DEST"
cp -R "$BUNDLE" "$DEST"
chown -R 0:0 "$DEST"
chmod -R 755 "$DEST"
echo "installed $DEST"

# kernelmanagerd checks the consent database before it will touch a kext, and `kmutil load -z`
# does not reliably create the row itself. Seed it the same way the system does: copy the
# shape of an existing unsigned entry, overriding only the identifier and display name.
sqlite3 "$POLICY" "INSERT OR REPLACE INTO kext_policy (team_id, bundle_id, allowed, developer_name, flags)
                   SELECT team_id, '$BUNDLE_ID', 1, 'Unidentified - $NAME', flags
                   FROM kext_policy WHERE allowed = 1 LIMIT 1;" 2>/dev/null || true
echo "consent row:"
sqlite3 "$POLICY" "select bundle_id, allowed from kext_policy;" 2>/dev/null || true

kmutil clear-staging >/dev/null 2>&1 || true
kmutil load -z -p "$DEST"

echo "--- verify ---"
if kmutil showloaded | grep -qi "$NAME"; then
    echo "LOADED"
else
    echo "not loaded. Run: kextutil -v $DEST   (it prints the real error; kmutil hides it)"
fi
