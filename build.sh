#!/bin/sh
# Builds a universal (Apple Silicon + Intel) Evict.app in .build/, plus .build/Evict.dmg with "./build.sh dmg".
# Signs with the "Developer ID Application" identity found in the keychain (ad-hoc otherwise), and notarizes
# the DMG when credentials exist: keychain profile "evict" locally, or NOTARY_APPLE_ID / NOTARY_PASSWORD /
# NOTARY_TEAM_ID in CI.
set -e
cd "$(dirname "$0")"
app=.build/Evict.app
rm -rf "$app" .build/tmp
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources" .build/tmp
echo "→ Building the scanner"
cc -O2 -arch arm64 -arch x86_64 -mmacosx-version-min=13.0 -o "$app/Contents/MacOS/evict-scan" scan.c
echo "→ Building the app"
for arch in arm64 x86_64; do
  xcrun swiftc -O -swift-version 5 -parse-as-library -target "$arch-apple-macos13" -o ".build/tmp/Evict-$arch" macos/Evict.swift
done
lipo -create -output "$app/Contents/MacOS/Evict" .build/tmp/Evict-arm64 .build/tmp/Evict-x86_64
cp macos/Info.plist "$app/Contents/"
cp macos/AppIcon.icns "$app/Contents/Resources/"
echo "→ Minifying the page"
python3 minify.py index.html "$app/Contents/Resources/index.html" 2>/dev/null || {
  echo "  minifier unavailable (needs Node): readable page copied as is"
  cp index.html "$app/Contents/Resources/"
}

identity=${EVICT_SIGN_IDENTITY:-$(security find-identity -v -p codesigning | awk -F'"' '/Developer ID Application/ {print $2; exit}')}
if [ -n "$identity" ]; then
  echo "→ Signing with $identity"
  # Inner executable first, then the bundle; hardened runtime is required for notarization
  codesign --force --options runtime --timestamp --sign "$identity" "$app/Contents/MacOS/evict-scan"
  codesign --force --options runtime --timestamp --sign "$identity" "$app"
else
  echo "→ No Developer ID certificate: ad-hoc signature"
  codesign --force --deep --sign - "$app"
fi

[ "$1" = dmg ] || exit 0
echo "→ Creating the DMG"
# The Finder lays out the window itself (background, icon positions), so any macOS version reads it back.
# The volume must mount as "Evict": the background is remembered by volume name.
for v in /Volumes/Evict /Volumes/Evict\ [0-9]*; do [ -d "$v" ] && hdiutil detach -force -quiet "$v"; done
rm -f .build/Evict.dmg .build/rw.dmg
hdiutil create -quiet -volname Evict -fs HFS+ -size 64m .build/rw.dmg
vol=/Volumes/Evict
hdiutil attach -quiet -readwrite -noverify -noautoopen .build/rw.dmg
[ -d "$vol" ] || { echo "The disk image didn't mount at $vol"; exit 1; }
cp -R "$app" "$vol/"
ln -s /Applications "$vol/Applications"
mkdir "$vol/.background"
cp macos/dmg-background.tiff "$vol/.background/background.tiff"
cp macos/AppIcon.icns "$vol/.VolumeIcon.icns"
xcrun SetFile -a C "$vol"
# A Finder that can't be driven (no GUI session) still gives a working DMG, with a plain window
osascript <<'APPLESCRIPT' || echo "  Finder unavailable: plain window layout"
tell application "Finder"
  tell disk "Evict"
    open
    set current view of container window to icon view
    set toolbar visible of container window to false
    set statusbar visible of container window to false
    set sidebar width of container window to 0
    -- 720×440 of content plus the title bar
    set bounds of container window to {200, 120, 920, 592}
    set opts to icon view options of container window
    set arrangement of opts to not arranged
    set icon size of opts to 128
    set text size of opts to 13
    set shows item info of opts to false
    -- The Finder picks the label color from the background color, not the picture: dark gives white labels
    set background color of opts to {2827, 3341, 4369}
    set background picture of opts to file ".background:background.tiff"
    set position of item "Evict.app" of container window to {190, 214}
    set position of item "Applications" of container window to {530, 214}
    set extension hidden of item "Evict.app" to true
    close
    open
    update without registering applications
    delay 2
    close
  end tell
end tell
APPLESCRIPT
sync
rm -rf "$vol/.fseventsd"
hdiutil detach -quiet "$vol"
hdiutil convert -quiet .build/rw.dmg -format UDZO -imagekey zlib-level=9 -o .build/Evict.dmg
rm -f .build/rw.dmg

if [ -n "$identity" ]; then
  codesign --force --timestamp --sign "$identity" .build/Evict.dmg
  if [ -n "$NOTARY_PASSWORD" ]; then
    notary="--apple-id $NOTARY_APPLE_ID --team-id $NOTARY_TEAM_ID --password $NOTARY_PASSWORD"
  elif xcrun notarytool history --keychain-profile evict > /dev/null 2>&1; then
    notary="--keychain-profile evict"
  fi
  if [ -n "$notary" ]; then
    # Usually a few minutes, but Apple can take much longer (first submissions of a new account especially)
    echo "→ Notarizing"
    # shellcheck disable=SC2086
    xcrun notarytool submit .build/Evict.dmg $notary --wait --timeout 3h | tee .build/notary.log
    grep -q 'status: Accepted' .build/notary.log || { echo "Notarization failed"; exit 1; }
    xcrun stapler staple .build/Evict.dmg
  else
    echo "→ No notarization credentials: DMG signed but not notarized"
  fi
fi
echo "✓ .build/Evict.dmg"
