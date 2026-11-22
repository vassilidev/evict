#!/bin/sh
# Evict's whole life on one distribution, as root in a fresh container: install with the one-line installer, start
# the app on a virtual display (it must scan, write its report and keep its web view), then uninstall and check
# that nothing is left. CI runs it on every supported distribution.
# Usage: linux/smoke.sh <package file>   a package built by CI, through docs/install.sh
#        linux/smoke.sh                  the published release, through the website's installer (EVICT_VERSION)
set -eu
cd "$(dirname "$0")/.."
file=${1:-}
site=https://vassilidev.github.io/evict/install.sh
log=/tmp/evict-smoke.log
fail() { [ -f "$log" ] && tail -n 30 "$log"; echo "FAIL: $*"; exit 1; }
quiet() { "$@" > /dev/null 2>&1 || { "$@"; fail "$*"; }; }
alive() { grep -qx "$1" /proc/[0-9]*/comm 2> /dev/null; }

# A virtual display, a session bus and curl; package names differ by family
if command -v apt-get > /dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  quiet apt-get update -qq
  quiet apt-get install -y -qq xvfb dbus curl ca-certificates
  # The tarball doesn't bring WebKitGTK: install it the way such a user would
  case $file in *.tar.gz) quiet apt-get install -y -qq libwebkit2gtk-4.1-0 ;; esac
elif command -v dnf > /dev/null; then
  dnf install -y xorg-x11-server-Xvfb dbus-daemon > /dev/null 2>&1 || nodisplay=1  # RHEL 10 and its rebuilds dropped the X server
elif command -v zypper > /dev/null; then
  # dbus-run-session is in dbus-1-daemon on Tumbleweed and Leap 16, in dbus-1 on Leap 15
  quiet sh -c 'zypper -n install xorg-x11-server-Xvfb dbus-1-daemon || zypper -n install xorg-x11-server-Xvfb dbus-1'
elif command -v pacman > /dev/null; then
  # Container images lag behind the repositories: upgrade fully first, like any Arch system (no partial upgrades)
  quiet pacman -Syu --noconfirm --needed xorg-server-xvfb dbus
fi

installer() {
  if [ -n "$file" ]; then EVICT_FILE="$file" sh docs/install.sh "$@"; else curl -fsSL "$site" | sh -s -- "$@"; fi
}

# Install
installer > "$log" 2>&1 || fail "install"
grep -q "✓ Evict" "$log" || fail "the installer didn't confirm"
if [ -e /usr/bin/evict ]; then bin=$(readlink -f /usr/bin/evict); else bin=$(readlink -f "$HOME/.local/bin/evict"); fi
version=$("$bin" --version)
echo "$version"
[ -z "${EVICT_VERSION:-}" ] || [ "$version" = "Evict ${EVICT_VERSION#v}" ] || fail "expected Evict ${EVICT_VERSION#v}"
[ -f /usr/share/applications/com.vassilidev.evict.desktop ] || [ -f "$HOME/.local/share/applications/com.vassilidev.evict.desktop" ] ||
  fail "no applications menu entry"

# Start
if [ -n "${nodisplay:-}" ]; then
  ! ldd "$bin" | grep 'not found' || fail "missing libraries"
  "$(dirname "$bin")/evict-scan" / > /dev/null 2>&1 || fail "scanner"
  started="libraries and scanner checked, no X server"
else
  Xvfb :99 -screen 0 1280x800x24 > /dev/null 2>&1 &
  sleep 2
  rm -rf "$HOME/.cache/evict"
  DISPLAY=:99 dbus-run-session -- "$bin" > "$log" 2>&1 &
  for _ in $(seq 60); do [ -s "$HOME/.cache/evict/evict.html" ] && break; sleep 1; done
  sleep 3
  [ -s "$HOME/.cache/evict/evict.html" ] || fail "no report"
  alive evict || fail "the app quit"
  # WebKit swaps its web process when the scan screen gives way to the map: give it a moment
  for _ in 1 2 3 4 5 6 7 8 9 10; do alive WebKitWebProces && break; sleep 1; done
  alive WebKitWebProces || echo "warning: no web process right now (WebKit may be between two pages)"
  for p in /proc/[0-9]*; do [ "$(cat "$p/comm" 2> /dev/null)" = evict ] && kill "${p#/proc/}"; done
  started="started, scanned, map shown"
fi

# Uninstall: nothing may be left
installer --uninstall > "$log" 2>&1 || fail "uninstall"
for f in /usr/bin/evict /usr/lib/evict /usr/share/applications/com.vassilidev.evict.desktop \
  /usr/share/icons/hicolor/512x512/apps/com.vassilidev.evict.png /usr/share/polkit-1/actions/com.vassilidev.evict.policy \
  "$HOME/.local/bin/evict" "$HOME/.local/lib/evict" "$HOME/.local/share/applications/com.vassilidev.evict.desktop" \
  "$HOME/.local/share/icons/hicolor/512x512/apps/com.vassilidev.evict.png" "$HOME/.cache/evict"; do
  [ ! -e "$f" ] || fail "left behind after uninstall: $f"
done
echo "OK $(. /etc/os-release && echo "$PRETTY_NAME"), $(uname -m): installed, $started, uninstalled cleanly"
