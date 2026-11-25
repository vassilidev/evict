#!/bin/sh
# Installs Evict from the DMG like a user (drag to Applications), starts it, quits it and removes it.
# CI runs it on the DMG it builds. Usage: macos/smoke.sh .build/Evict.dmg
set -eu
fail() { echo "FAIL: $*"; exit 1; }
# It replaces /Applications/Evict.app: never someone's own copy
[ -n "${CI:-}" ] || [ ! -e /Applications/Evict.app ] || fail "an Evict is already in /Applications: run this on CI only"
mnt=$(mktemp -d)
hdiutil attach -quiet -nobrowse -readonly -mountpoint "$mnt" "$1"
[ -d "$mnt/Evict.app" ] || fail "no Evict.app in the DMG"
[ "$(readlink "$mnt/Applications")" = /Applications ] || fail "no Applications link in the DMG"
[ -f "$mnt/.background/background.tiff" ] || fail "no window background in the DMG"
rm -rf /Applications/Evict.app
ditto "$mnt/Evict.app" /Applications/Evict.app
hdiutil detach -quiet "$mnt"
codesign --verify --deep --strict /Applications/Evict.app || fail "signature"
version=$(defaults read /Applications/Evict.app/Contents/Info CFBundleShortVersionString)

open /Applications/Evict.app
for _ in $(seq 20); do pgrep -x Evict > /dev/null && break; sleep 1; done
sleep 5
pgrep -x Evict > /dev/null || fail "Evict didn't stay open"
osascript -e 'tell application "Evict" to quit' > /dev/null 2>&1 || true
for _ in $(seq 10); do pgrep -x Evict > /dev/null || break; sleep 1; done
! pgrep -x Evict > /dev/null || fail "Evict didn't quit"

# What the LaunchAgent runs: a scan for the history (only with Full Disk Access), then it quits, with no window
/Applications/Evict.app/Contents/MacOS/Evict --background || fail "the background scan failed"
! pgrep -x Evict > /dev/null || fail "the background scan didn't quit"

rm -rf /Applications/Evict.app
[ ! -e /Applications/Evict.app ] || fail "couldn't remove the app"
echo "OK macOS $(sw_vers -productVersion), Evict $version: installed from the DMG, started, quit, removed"
