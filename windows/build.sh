#!/bin/sh
# Builds Evict for Windows in .build/windows/<arch>/ (x64, arm64 and x86): the app (Win32 + WebView2), the scanner,
# the page and Microsoft's WebView2 loader. "windows/build.sh package" also makes .build/Evict-Setup.exe, one installer
# for the three architectures that installs for the current user, no administrator rights needed.
# Cross-compiles with llvm-mingw in an Ubuntu 22.04 container: runs from macOS or Linux with Docker.
set -e
cd "$(dirname "$0")/.."
if [ -z "$EVICT_CONTAINER" ]; then
  mkdir -p .build/cache
  python3 minify.py index.html .build/index.min.html 2> /dev/null || rm -f .build/index.min.html
  exec docker run --rm -e EVICT_CONTAINER=1 -v "$PWD:/src" -w /src ubuntu:22.04 sh windows/build.sh "$@"
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends curl ca-certificates xz-utils unzip nsis > /dev/null

# Pinned toolchain and SDK, checked against their SHA-256
mingw=20261020
case $(uname -m) in
  x86_64) sum=55064b60b991a1e1e8090b8c2f84e82b22cfe5903d12c159d65064cf5e8ae335 ;;
  aarch64) sum=ca16bb0f4f485876496fa46ad8d353c525eba57d0cf9e9e49ec1738fe4810151 ;;
esac
name=llvm-mingw-$mingw-ucrt-ubuntu-22.04-$(uname -m)
[ -f ".build/cache/$name.tar.xz" ] ||
  curl -fsSL -o ".build/cache/$name.tar.xz" "https://github.com/mstorsjo/llvm-mingw/releases/download/$mingw/$name.tar.xz"
echo "$sum  .build/cache/$name.tar.xz" | sha256sum -c - > /dev/null
tar -xJf ".build/cache/$name.tar.xz" -C /opt
export PATH="/opt/$name/bin:$PATH"
sdk=1.0.4314.35
nupkg=.build/cache/webview2-$sdk.nupkg
[ -f "$nupkg" ] || curl -fsSL -o "$nupkg" "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$sdk/microsoft.web.webview2.$sdk.nupkg"
echo "90778deb1b77e36292828b4b5833c9255c27533f40e0ddd55891ae718944edc4  $nupkg" | sha256sum -c - > /dev/null
unzip -qo "$nupkg" 'build/native/*' -d /opt/webview2
# WebView2.h includes EventToken.h, which MinGW names in lowercase
mkdir -p /opt/include
echo '#include <eventtoken.h>' > /opt/include/EventToken.h

version=$(sed -n '/CFBundleShortVersionString/{n;s/.*<string>\(.*\)<\/string>.*/\1/p;}' macos/Info.plist)
# Resource details for each program, through a header (string defines don't survive the resource compiler's command line)
details() {
  mkdir -p "/opt/res/$1"
  printf '#define VERSION "%s"\n#define VERSION_NUMS %s,0\n#define FILE_DESCRIPTION "%s"\n#define ORIGINAL_NAME "%s"\n' \
    "$version" "$(echo "$version" | tr . ,)" "$2" "$1" > "/opt/res/$1/details.h"
}
details Evict.exe Evict
details evict-scan.exe "Evict disk scanner"

if [ .build/index.min.html -nt index.html ]; then page=.build/index.min.html; else page=index.html; fi
[ $page = index.html ] && echo "  minifier unavailable (needs Node): readable page copied as is"
rm -rf .build/windows
warn="-Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers"
for arch in x64 arm64 x86; do
  case $arch in x64) cc=x86_64 ;; arm64) cc=aarch64 ;; x86) cc=i686 ;; esac
  out=.build/windows/$arch
  mkdir -p "$out"
  echo "→ Building for $arch"
  for exe in Evict.exe evict-scan.exe; do
    $cc-w64-mingw32-windres -I"/opt/res/$exe" -Iwindows windows/evict.rc -O coff -o "/opt/res/$exe/$arch.res"
  done
  $cc-w64-mingw32-clang -O2 -static -pthread $warn -o "$out/evict-scan.exe" scan.c /opt/res/evict-scan.exe/$arch.res \
    -lshell32 -ladvapi32
  $cc-w64-mingw32-clang -O2 -static -municode -mwindows $warn -Wno-unknown-pragmas -I/opt/webview2/build/native/include -I/opt/include \
    -DVERSION="\"$version\"" -o "$out/Evict.exe" windows/evict.c /opt/res/Evict.exe/$arch.res \
    -lole32 -lshell32 -lshlwapi -ldwmapi -lcomctl32 -lbcrypt -lurlmon -luuid -ladvapi32
  $cc-w64-mingw32-strip "$out/Evict.exe" "$out/evict-scan.exe"
  cp "/opt/webview2/build/native/$arch/WebView2Loader.dll" "$out/"
  cp $page "$out/index.html"
done
echo "✓ .build/windows (Evict $version)"

[ "$1" = package ] || exit 0
echo "→ Packaging"
makensis -V2 -DVERSION="$version" windows/installer.nsi
ls -l .build/Evict-Setup.exe
