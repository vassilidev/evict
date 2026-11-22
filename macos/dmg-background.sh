#!/bin/sh
# Renders macos/dmg-background.html into macos/dmg-background.tiff (1x + 2x for Retina) with Chrome. Run it after
# editing the HTML; the build only uses the TIFF.
set -e
cd "$(dirname "$0")"
chrome="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for scale in 1 2; do
  "$chrome" --headless=new --hide-scrollbars --window-size=720,440 --force-device-scale-factor=$scale \
    --screenshot="$tmp/bg$scale.png" "file://$PWD/dmg-background.html" 2> /dev/null
done
tiffutil -cathidpicheck "$tmp/bg1.png" "$tmp/bg2.png" -out dmg-background.tiff
echo "✓ macos/dmg-background.tiff"
