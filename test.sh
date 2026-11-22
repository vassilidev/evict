#!/bin/sh
# Checks the scanner total against du on a small fixture (big file, small files, hard link)
set -e
cd "$(dirname "$0")"
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
mkdir -p "$t/f/a/b" "$t/f/c"
dd if=/dev/urandom of="$t/f/a/big" bs=1048576 count=3 2>/dev/null
dd if=/dev/urandom of="$t/f/a/b/small" bs=1024 count=10 2>/dev/null
ln "$t/f/a/big" "$t/f/c/hardlink"
echo hi > "$t/f/c/tiny"
cc -O2 -pthread -Wall -Wextra -Werror -o "$t/scan" scan.c
json=$("$t/scan" "$t/f" 2>/dev/null)
got=$(printf '%s' "$json" | sed -E 's/.*"tree":\["[^"]*",([0-9]+).*/\1/')
want=$(( $(du -sk "$t/f" | cut -f1) * 1024 ))
printf '%s' "$json" | grep -qE '\["(big|hardlink)",' || { echo "FAIL: big file missing"; exit 1; }
[ "$got" = "$want" ] || { echo "FAIL: scan=$got du=$want"; exit 1; }
# System folders: the first scan walks them and fills the cache, the second one reads it; same total both times
mkdir -p "$t/Library/Caches"
tot() { "$t/scan" --home "$t" /usr 2>/dev/null | sed -E 's/.*"tree":\["[^"]*",([0-9]+).*/\1/'; }
walked=$(tot)
cached=$(tot)
[ "$walked" = "$cached" ] || { echo "FAIL: /usr walked=$walked cached=$cached"; exit 1; }
echo "OK ($got bytes, same as du)"
