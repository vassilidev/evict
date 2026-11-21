#!/bin/sh
# Builds a universal (Apple Silicon + Intel) Evict.app in .build/, plus .build/Evict.dmg with "./build.sh dmg"
set -e
cd "$(dirname "$0")"
app=.build/Evict.app
rm -rf "$app" .build/tmp
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources" .build/tmp
echo "→ Compilation du scanner"
cc -O2 -arch arm64 -arch x86_64 -mmacosx-version-min=13.0 -o "$app/Contents/MacOS/evict-scan" scan.c
echo "→ Compilation de l'app"
for arch in arm64 x86_64; do
  xcrun swiftc -O -swift-version 5 -parse-as-library -target "$arch-apple-macos13" -o ".build/tmp/Evict-$arch" app/Evict.swift
done
lipo -create -output "$app/Contents/MacOS/Evict" .build/tmp/Evict-arm64 .build/tmp/Evict-x86_64
cp app/Info.plist "$app/Contents/"
cp index.html app/AppIcon.icns "$app/Contents/Resources/"
codesign --force --deep --sign - "$app"

if [ "$1" = dmg ]; then
  echo "→ Création du DMG"
  rm -rf .build/dmg .build/Evict.dmg
  mkdir .build/dmg
  cp -R "$app" .build/dmg/
  ln -s /Applications .build/dmg/Applications
  hdiutil create -quiet -volname Evict -srcfolder .build/dmg -format UDZO .build/Evict.dmg
  echo "✓ .build/Evict.dmg"
fi
