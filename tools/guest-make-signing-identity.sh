#!/bin/sh
#
#  guest-make-signing-identity.sh — create a self-signed code-signing identity
#  so that macOS has a team identifier to key kext approval on.
#
#  Why this is needed
#  ------------------
#  Apple TN2459: "Approved KEXTs are tracked in a system-wide policy database
#  through the team identifier in the KEXT's code signature and the bundle
#  identifier from the KEXT's Info.plist." A kext with no signature has no team
#  identifier, so there is nothing for the approval to be recorded against.
#  Measured on this guest: an unsigned kext is refused by syspolicyd with
#  "Unsigned kext not present in the legacy kext list", System Settings still
#  offers an Allow button, the user can press it, and nothing changes --
#  because `kext_policy` rows are keyed on team_id and there is no team_id.
#  Hand-inserting a row with an empty team_id does not help either.
#
#  A real Developer ID would be Apple-issued and notarised. This produces a
#  self-signed certificate whose OU becomes the team identifier, which is
#  enough for the approval machinery to have something to record. On a guest
#  with SIP disabled and no AMFI enforcement the signature also satisfies the
#  kernel. It is not equivalent to an Apple signature and is not shippable.
#
#  Usage (as root, on the guest):
#      sh guest-make-signing-identity.sh [CN] [OU] [keychain-path]
#  Then:
#      codesign --force --keychain <kc> --sign <CN> /Library/Extensions/x.kext
#      codesign -dvvv /Library/Extensions/x.kext      # shows TeamIdentifier
#
set -eu

CN=${1:-vgpu-macos}
OU=${2:-VGPU000001}
KC=${3:-/var/tmp/vgpu.keychain}
KC_PASS=vgpu-keychain
P12_PASS=vgpu-p12
WORK=/var/tmp/vgpu-identity

mkdir -p "$WORK"
cd "$WORK"

echo "--- 1. openssl config ---"
cat > identity.cnf <<EOF
[req]
distinguished_name = dn
x509_extensions    = v3
prompt             = no

[dn]
CN = $CN
OU = $OU
O  = $CN

[v3]
basicConstraints       = critical,CA:FALSE
keyUsage               = critical,digitalSignature
extendedKeyUsage       = critical,codeSigning
subjectKeyIdentifier   = hash
EOF
echo "wrote $WORK/identity.cnf"

echo "--- 2. key pair and certificate ---"
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
    -keyout identity.key -out identity.crt -config identity.cnf 2>&1 | tail -3
# Note: keep these diagnostics tolerant. macOS ships LibreSSL, whose `x509`
# has no -ext flag -- and that unsupported option aborts the whole script
# under `set -e`, silently skipping the keychain steps below.
openssl x509 -in identity.crt -noout -subject -nameopt RFC2253 || true
openssl x509 -in identity.crt -noout -text 2>/dev/null | grep -i -A1 'extended key usage' || true

echo "--- 3. keychain ---"
security create-keychain -p "$KC_PASS" "$KC" 2>/dev/null || true
security unlock-keychain -p "$KC_PASS" "$KC"
security set-keychain-settings "$KC"
echo "keychain ready: $KC"

echo "--- 4. import the identity ---"
openssl pkcs12 -export -inkey identity.key -in identity.crt \
    -out identity.p12 -passout pass:"$P12_PASS" -name "$CN" 2>&1 | tail -2
security import identity.p12 -k "$KC" -P "$P12_PASS" -A -T /usr/bin/codesign 2>&1 | tail -3
# Without this, codesign blocks on a GUI "allow access" prompt that a
# headless guest can never answer.
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$KC_PASS" "$KC" >/dev/null 2>&1

echo "--- 5. trust the certificate for code signing ---"
# An imported but untrusted identity does not count as valid: codesign then
# fails with "The specified item could not be found in the keychain" even
# though the import reported success. Trusting it for the codeSign policy is
# what makes it usable. -d writes the admin trust settings, which needs root.
security add-trusted-cert -d -r trustRoot -p codeSign \
    -k /Library/Keychains/System.keychain identity.crt 2>&1 | tail -3 || true

echo "--- 6. identities now available ---"
security find-identity -v -p codesigning "$KC" 2>&1 | tail -5

echo
echo "Team identifier to expect: $OU"
echo "Sign with:  codesign --force --keychain $KC --sign $CN <bundle>"
