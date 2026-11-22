#!/bin/sh
# Builds Evict from source and installs it: in /Applications (or ~/Applications) on macOS, in ~/.local on Linux
set -e
cd "$(dirname "$0")"
if [ "$(uname -s)" = Linux ]; then
  linux/build.sh package
  EVICT_FILE=".build/evict-linux-$(uname -m).tar.gz" sh docs/install.sh
  exit
fi
./build.sh

dest=/Applications
[ -w "$dest" ] || dest="$HOME/Applications"
mkdir -p "$dest"
pkill -x Evict 2>/dev/null || true
rm -rf "$dest/Evict.app"
cp -R .build/Evict.app "$dest/"
echo "✓ Installed: $dest/Evict.app"
open "$dest/Evict.app"
