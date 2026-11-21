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
  xcrun swiftc -O -swift-version 5 -parse-as-library -target "$arch-apple-macos13" -o ".build/tmp/Evict-$arch" app/Evict.swift
done
lipo -create -output "$app/Contents/MacOS/Evict" .build/tmp/Evict-arm64 .build/tmp/Evict-x86_64
cp app/Info.plist "$app/Contents/"
cp index.html app/AppIcon.icns "$app/Contents/Resources/"

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
rm -rf .build/dmg .build/Evict.dmg
mkdir .build/dmg
cp -R "$app" .build/dmg/
ln -s /Applications .build/dmg/Applications
hdiutil create -quiet -volname Evict -srcfolder .build/dmg -format UDZO .build/Evict.dmg

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
