#!/bin/sh
# Builds Evict.app from source and installs it in /Applications (or ~/Applications)
set -e
cd "$(dirname "$0")"
app=.build/Evict.app
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
echo "→ Compilation du scanner"
cc -O2 -o "$app/Contents/MacOS/evict-scan" scan.c
echo "→ Compilation de l'app"
xcrun swiftc -O -swift-version 5 -parse-as-library -o "$app/Contents/MacOS/Evict" app/Evict.swift
cp app/Info.plist "$app/Contents/"
cp index.html "$app/Contents/Resources/"
cp app/AppIcon.icns "$app/Contents/Resources/"
codesign --force --deep --sign - "$app"

dest=/Applications
[ -w "$dest" ] || dest="$HOME/Applications"
mkdir -p "$dest"
pkill -x Evict 2>/dev/null || true
rm -rf "$dest/Evict.app"
cp -R "$app" "$dest/"
echo "✓ Installé : $dest/Evict.app"
open "$dest/Evict.app"
