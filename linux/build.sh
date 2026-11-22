#!/bin/sh
# Builds Evict for Linux in .build/linux/evict: the app (GTK 3 + WebKitGTK 4.1), the scanner and the page.
# "linux/build.sh package" also makes the release files in .build/: evict-linux-<arch>.tar.gz, plus the .deb,
# .rpm and Arch packages when nfpm is installed (CI runs it from its Docker image).
# Needs a C compiler and WebKitGTK 4.1 or 4.0 headers: libwebkit2gtk-4.1-dev (Debian, Ubuntu), webkit2gtk4.1-devel
# (Fedora), webkit2gtk-4.1 (Arch). The app loads either version at startup, whichever the system has.
set -e
cd "$(dirname "$0")/.."
version=$(sed -n '/CFBundleShortVersionString/{n;s/.*<string>\(.*\)<\/string>.*/\1/p;}' macos/Info.plist)
arch=$(uname -m)
out=.build/linux/evict
rm -rf .build/linux
mkdir -p "$out"
echo "→ Building the scanner"
cc -O2 -pthread -o "$out/evict-scan" scan.c
echo "→ Building the app"
# shellcheck disable=SC2046
cc -O2 -Wall -Wno-deprecated-declarations -DVERSION="\"$version\"" -o "$out/evict" linux/evict.c \
  $(pkg-config --cflags webkit2gtk-4.1 2> /dev/null || pkg-config --cflags webkit2gtk-4.0) $(pkg-config --libs gtk+-3.0) -ldl
if [ .build/index.min.html -nt index.html ]; then
  cp .build/index.min.html "$out/index.html"  # minified beforehand where Node is available (CI)
else
  echo "→ Minifying the page"
  python3 minify.py index.html "$out/index.html" 2>/dev/null || {
    echo "  minifier unavailable (needs Node): readable page copied as is"
    cp index.html "$out/"
  }
fi
cp linux/com.vassilidev.evict.desktop linux/com.vassilidev.evict.png "$out/"
echo "✓ $out (Evict $version, $arch)"

[ "$1" = package ] || exit 0
echo "→ Packaging"
tar -czf ".build/evict-linux-$arch.tar.gz" -C .build/linux evict
if command -v nfpm > /dev/null; then
  goarch=$([ "$arch" = aarch64 ] && echo arm64 || echo amd64)
  for kind in deb rpm archlinux; do
    ext=$([ $kind = archlinux ] && echo pkg.tar.zst || echo $kind)
    ARCH=$goarch VERSION=$version nfpm package -f linux/nfpm.yaml -p $kind -t ".build/evict-linux-$arch.$ext" > /dev/null
  done
fi
ls -1 .build/evict-linux-*
