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
# History: each scan adds a line, the report carries them; --alert compares with the scan before. Root never writes
# it (the Linux app's administrator scan runs as root in the user's folder)
[ "$(id -u)" != 0 ] || { echo "OK ($got bytes, same as du; history not checked as root)"; exit 0; }
mkdir -p "$t/home"
hist() { XDG_DATA_HOME= "$t/scan" --home "$t/home" "$@" 2>/dev/null; }
hist --history "$t/f" > /dev/null
n=$(hist --history "$t/f" | grep -o '{"r":' | wc -l | tr -d ' ')
[ "$n" = 2 ] || { echo "FAIL: history has $n scans, expected 2"; exit 1; }
file=$(find "$t/home" -name history.jsonl)
last=$(tail -n 1 "$file")
free=$(printf '%s' "$last" | sed -E 's/.*"free":([0-9]+).*/\1/')
size=$(printf '%s' "$last" | sed -E 's/.*"total":([0-9]+).*/\1/')
# More than the 10 GB or 2% of the disk that make an alert
printf '%s\n' "$last" | sed -E "s/\"free\":[0-9]+/\"free\":$((free + 20000000000 + size / 40))/" >> "$file"
hist --alert "$t/f" | grep -q '^Your disk grew by ' || { echo "FAIL: no alert after the disk grew"; exit 1; }
[ -z "$(hist --alert "$t/f")" ] || { echo "FAIL: alert without growth"; exit 1; }
echo "OK ($got bytes, same as du)"
