#!/bin/sh
# Builds Evict.app from source and installs it in /Applications (or ~/Applications)
set -e
cd "$(dirname "$0")"
./build.sh

dest=/Applications
[ -w "$dest" ] || dest="$HOME/Applications"
mkdir -p "$dest"
pkill -x Evict 2>/dev/null || true
rm -rf "$dest/Evict.app"
cp -R .build/Evict.app "$dest/"
echo "✓ Installé : $dest/Evict.app"
open "$dest/Evict.app"
