#!/bin/sh
# Headless Command Line Tools install for the vGPU-macOS test guest.
#
# Why not `xcode-select --install`: that only opens a GUI dialog on the console and
# cannot be accepted over SSH. Instead we drop Apple's "install on demand" flag, which
# makes `softwareupdate --list` advertise the Command Line Tools package, and install
# it from the command line.
#
# Usage (as root):  sh guest-install-clt.sh ["Command Line Tools for Xcode-16.4"]

set -u

LABEL="${1:-Command Line Tools for Xcode-16.4}"
LOG=/var/tmp/clt-install.log
FLAG=/tmp/.com.apple.dt.CommandLineTools.installondemand.in-progress

# Drop any GUI installer the console may have started, so the two paths don't race.
pkill -f "Install Command Line Developer Tools" 2>/dev/null || true

touch "$FLAG"
trap 'rm -f "$FLAG"' EXIT INT TERM

{
    echo "=== guest-install-clt.sh $(date) ==="
    echo "target: $LABEL"
    echo "--- softwareupdate --list (filtered) ---"
    softwareupdate --list 2>&1 | grep -i -A2 'command line tools' || echo "(CLT not advertised)"
    echo "--- installing ---"
} > "$LOG" 2>&1

softwareupdate -i "$LABEL" --agree-to-license --verbose >> "$LOG" 2>&1
rc=$?

{
    echo "--- finished rc=$rc ---"
    if [ -x /Library/Developer/CommandLineTools/usr/bin/clang ]; then
        /Library/Developer/CommandLineTools/usr/bin/clang --version
        echo "CLT_OK"
    else
        echo "CLT_MISSING"
    fi
} >> "$LOG" 2>&1

exit $rc
