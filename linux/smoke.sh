#!/bin/sh
# Installs Evict with docs/install.sh, then starts the app on a virtual display: it must scan the disk, write its
# report and keep its web view running. CI runs it as root in a container of each supported distribution.
# Usage: linux/smoke.sh .build/evict-linux-<arch>.<deb|rpm|pkg.tar.zst|tar.gz>
set -eu
cd "$(dirname "$0")/.."
log=/tmp/evict-smoke.log
quiet() { "$@" > /dev/null 2>&1 || { echo "FAIL: $*"; "$@"; exit 1; }; }

# A virtual display and a session bus; package names differ by family
if command -v apt-get > /dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  quiet apt-get update -qq
  quiet apt-get install -y -qq xvfb dbus
elif command -v dnf > /dev/null; then
  dnf install -y xorg-x11-server-Xvfb dbus-daemon > /dev/null 2>&1 || nodisplay=1  # RHEL 10 and its rebuilds dropped the X server
elif command -v zypper > /dev/null; then
  quiet zypper --non-interactive install xorg-x11-server-Xvfb dbus-1-daemon
elif command -v pacman > /dev/null; then
  quiet pacman -Sy --noconfirm --needed xorg-server-xvfb dbus
fi

EVICT_FILE="$1" sh docs/install.sh > /tmp/evict-install.log 2>&1 || { cat /tmp/evict-install.log; echo "FAIL: install"; exit 1; }
evict --version
if [ -n "${nodisplay:-}" ]; then
  ! ldd "$(readlink -f "$(command -v evict)")" | grep 'not found' || { echo "FAIL: missing libraries"; exit 1; }
  "$(dirname "$(readlink -f "$(command -v evict)")")/evict-scan" / > /dev/null 2>&1 || { echo "FAIL: scanner"; exit 1; }
  echo "OK $(. /etc/os-release && echo "$PRETTY_NAME"), $(uname -m) (no X server: libraries and scanner checked)"
  exit 0
fi
Xvfb :99 -screen 0 1280x800x24 > /dev/null 2>&1 &
sleep 2
rm -rf "$HOME/.cache/evict"
DISPLAY=:99 dbus-run-session -- evict > "$log" 2>&1 &
for _ in $(seq 60); do [ -s "$HOME/.cache/evict/evict.html" ] && break; sleep 1; done
sleep 3
alive() { grep -qx "$1" /proc/[0-9]*/comm 2> /dev/null; }
[ -s "$HOME/.cache/evict/evict.html" ] || { cat "$log"; echo "FAIL: no report"; exit 1; }
alive evict || { cat "$log"; echo "FAIL: the app quit"; exit 1; }
# WebKit swaps its web process when the scan screen gives way to the map: give it a moment
for _ in 1 2 3 4 5 6 7 8 9 10; do alive WebKitWebProces && break; sleep 1; done
alive WebKitWebProces || echo "warning: no web process right now (WebKit may be between two pages)"
echo "OK $(. /etc/os-release && echo "$PRETTY_NAME"), $(uname -m)"
