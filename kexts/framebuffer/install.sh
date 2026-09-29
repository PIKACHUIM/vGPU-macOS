#!/bin/sh
#
#  install.sh — build, sign, authorise, stage and verify vgpuFramebuffer.
#
#  Everything after "build" exists because each step was measured to be necessary on a
#  macOS 15 guest with SIP disabled. See docs/kext-loading.md for the full account.
#
#    * /Library/Extensions and not /tmp — a staged entry records the path it came from and
#      kernelmanagerd re-checks it on every boot; /tmp is cleared on reboot.
#    * a self-signed identity whose certificate carries an OU — kext approval is recorded
#      against the team identifier in the code signature (Apple TN2459), so an unsigned kext
#      has nothing to be approved against. System Settings' Allow button silently does
#      nothing for one.
#    * a kext_policy row for that team identifier — with it syspolicyd reports
#      "Kernel Extension ALLOWED" and kernelmanagerd "Validate approval ... approved".
#    * kmutil load after installing — this is what makes kernelmanagerd rebuild the
#      auxiliary kernel collection at the next boot. Skipping it means the collection is
#      left as it was and the kext does not load. Whether it is included is otherwise not
#      deterministic, which is why this script verifies rather than assumes.
#
#  Usage, on the guest, as root:
#      sh install.sh              # build is expected at ./build
#
#  Recovery, if the console is lost: over ssh,
#      rm -rf /Library/Extensions/vgpuFramebuffer.kext && kmutil clear-staging && reboot
#  the network stack does not depend on the console.
#
set -eu

NAME=vgpuFramebuffer
BUNDLE_ID=com.vgpu.framebuffer
TEAM_ID=VGPU000001
IDENT_SHA=38EA4CAC4ADBC48D4665449BC54178328C35AC4C
KEYCHAIN=/var/tmp/vgpu.keychain
KC_PASS=vgpu-keychain
HERE=$(cd "$(dirname "$0")" && pwd)
BUNDLE="$HERE/build/$NAME.kext"
DEST="/Library/Extensions/$NAME.kext"
AUXKC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc

echo "--- 1. bundle sanity ---"
# A failed build leaves a bundle with an Info.plist and no executable, which
# kernelmanagerd then reports as a "codeless kext". Catch that here rather than
# debugging a "Could not find: Executable for Kext" after a reboot.
if [ ! -x "$BUNDLE/Contents/MacOS/$NAME" ]; then
    echo "no executable in $BUNDLE -- the build failed; do not install" >&2
    exit 1
fi
echo "executable present: $(ls -l "$BUNDLE/Contents/MacOS/$NAME" | awk '{print $5}') bytes"

echo "--- 2. install ---"
rm -rf "$DEST"
cp -R "$BUNDLE" "$DEST"
chown -R 0:0 "$DEST"
chmod -R 755 "$DEST"

echo "--- 3. sign ---"
security unlock-keychain -p "$KC_PASS" "$KEYCHAIN" >/dev/null 2>&1 || true
codesign --force --keychain "$KEYCHAIN" --sign "$IDENT_SHA" "$DEST"
codesign -dvvv "$DEST" 2>&1 | grep -iE 'Authority=|CDHash=' | head -2

echo "--- 4. authorise against the team identifier ---"
sqlite3 /var/db/SystemPolicyConfiguration/KextPolicy \
    "insert or replace into kext_policy (team_id,bundle_id,allowed,developer_name,flags)
     values ('$TEAM_ID','$BUNDLE_ID',1,'vgpu-macos',0);"
sqlite3 -header /var/db/SystemPolicyConfiguration/KextPolicy \
    "select team_id,bundle_id,allowed from kext_policy;"

echo "--- 5. ask for it to be built in ---"
kmutil clear-staging >/dev/null 2>&1 || true
# Without -z this stops at the consent gate and returns Code=27 even when the policy row
# is in place; -z gets the request to kernelmanagerd, which re-checks approval itself.
kmutil load -z -p "$DEST" 2>&1 | grep -oE 'Code=[0-9]+ .*' | head -1 \
    || echo "queued (no error)"

echo "--- 6. verify ---"
# The collection is rebuilt at the next boot, so a count of zero here is expected on the
# first pass. After a reboot it is the thing to check.
if [ -f "$AUXKC" ]; then
    COUNT=$(grep -a -c "$NAME" "$AUXKC" 2>/dev/null || true)
    echo "auxiliary collection contains $NAME: ${COUNT} occurrence(s)"
    if [ "${COUNT:-0}" -gt 0 ]; then
        echo "OK: staged and present in the collection; a reboot will load it"
    else
        echo "not in the collection yet; reboot and re-run to confirm it loaded"
    fi
else
    echo "no auxiliary collection on disk yet; reboot and re-run"
fi

kmutil showloaded 2>/dev/null | grep -i "$NAME" \
    || echo "(not loaded in the running kernel; expected unless this is a post-reboot check)"
