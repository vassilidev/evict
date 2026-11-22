#!/bin/sh
# Installs or updates Evict on Linux, through the distribution's package manager when there is one
# (apt, dnf, zypper, pacman), so WebKitGTK comes with it and the app shows up in the applications menu.
#
#   curl -fsSL https://vassilidev.github.io/evict/install.sh | sh                   install or update
#   curl -fsSL https://vassilidev.github.io/evict/install.sh | sh -s -- --uninstall
#
# EVICT_VERSION=1.11.0 installs that version; EVICT_FILE=path installs a package or tarball you already have.
set -eu

repo=vassilidev/evict
say() { printf '%s\n' "$*"; }
die() { printf 'Error: %s\n' "$*" >&2; exit 1; }
has() { command -v "$1" > /dev/null 2>&1; }

[ "$(uname -s)" = Linux ] || die "this installer is for Linux. For macOS: https://vassilidev.github.io/evict/download.html"

sudo=
if [ "$(id -u)" -ne 0 ]; then
  if has sudo; then sudo=sudo; elif has doas; then sudo=doas; fi
fi
root() {
  [ "$(id -u)" -eq 0 ] || [ -n "$sudo" ] || die "run this as root, or install sudo."
  # stdin is this script when piped into sh: package managers must not read it
  $sudo "$@" < /dev/null
}

lib="$HOME/.local/lib/evict"
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
icons="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/512x512/apps"

if [ "${1:-}" = --uninstall ]; then
  if has dpkg && dpkg -s evict > /dev/null 2>&1; then root apt-get remove -y evict
  elif has rpm && rpm -q evict > /dev/null 2>&1; then
    if has dnf; then root dnf remove -y evict; elif has zypper; then root zypper --non-interactive remove evict; else root rpm -e evict; fi
  elif has pacman && pacman -Q evict > /dev/null 2>&1; then root pacman -R --noconfirm evict
  fi
  rm -rf "$lib" "$HOME/.local/bin/evict" "$apps/com.vassilidev.evict.desktop" "$icons/com.vassilidev.evict.png" "$HOME/.cache/evict"
  say "✓ Evict removed"
  exit 0
fi

case $(uname -m) in
  x86_64 | amd64) arch=x86_64 ;;
  aarch64 | arm64) arch=aarch64 ;;
  *) die "Evict is built for x86_64 and ARM64, not $(uname -m)." ;;
esac
if ldd --version 2>&1 | grep -qi musl; then die "Evict needs a glibc distribution (musl isn't supported yet)."; fi

if [ -n "${EVICT_FILE:-}" ]; then
  kind=${EVICT_FILE##*/}
  kind=${kind#evict-linux-*.}
elif has apt-get; then kind=deb
elif has dnf || has zypper || has yum; then kind=rpm
elif has pacman; then kind=pkg.tar.zst
else kind=tar.gz
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
chmod 755 "$tmp"  # apt reads the package as its own sandbox user
file="$tmp/evict-linux-$arch.$kind"

if [ -n "${EVICT_FILE:-}" ]; then
  cp "$EVICT_FILE" "$file"
  version=local
else
  has curl || die "curl is needed to download Evict."
  asset="evict-linux-$arch.$kind"
  if [ -n "${EVICT_VERSION:-}" ]; then
    url="https://github.com/$repo/releases/download/v${EVICT_VERSION#v}/$asset"
  else
    # Releases come newest first; the first one with this file is the latest for Linux (its macOS build can still be at Apple)
    url=$(curl -fsSL "https://api.github.com/repos/$repo/releases?per_page=10" 2> /dev/null |
      grep -o "\"browser_download_url\": *\"[^\"]*/$asset\"" | head -n 1 | sed 's/.*"\(https[^"]*\)"$/\1/') || true
    [ -n "$url" ] || url="https://github.com/$repo/releases/latest/download/$asset"
  fi
  version=$(printf '%s' "$url" | sed -n 's|.*/download/v\([^/]*\)/.*|\1|p')
  say "→ Downloading Evict ${version:-latest} for $arch"
  curl -fL --progress-bar -o "$file" "$url" || die "couldn't download $url"
  chmod 644 "$file"
fi

say "→ Installing"
case $kind in
  deb)
    # Package lists can be missing or stale (fresh systems, containers): refresh them once if WebKitGTK can't be found
    root apt-get install -y "$file" || { root apt-get update && root apt-get install -y "$file"; } ;;
  rpm)
    if has dnf; then
      root dnf install -y "$file" || {
        # Rocky, Alma, RHEL and CentOS Stream ship WebKitGTK 4.1 in EPEL
        grep -qi rhel /etc/os-release || exit 1
        say "→ Enabling EPEL for WebKitGTK"
        root dnf install -y epel-release && root dnf install -y "$file"
      }
    elif has zypper; then root zypper --non-interactive install --allow-unsigned-rpm "$file"
    else root yum install -y "$file"
    fi ;;
  pkg.tar.zst)
    root pacman -U --noconfirm --needed "$file" || die "pacman couldn't install it. Update your system (sudo pacman -Syu), then run this again." ;;
  tar.gz)
    rm -rf "$lib"
    mkdir -p "$HOME/.local/lib" "$HOME/.local/bin" "$apps" "$icons"
    tar -xzf "$file" -C "$HOME/.local/lib"
    ln -sf "$lib/evict" "$HOME/.local/bin/evict"
    sed "s|^Exec=evict$|Exec=$lib/evict|" "$lib/com.vassilidev.evict.desktop" > "$apps/com.vassilidev.evict.desktop"
    cp "$lib/com.vassilidev.evict.png" "$icons/"
    has update-desktop-database && update-desktop-database -q "$apps" 2> /dev/null || true
    if has ldconfig && ! ldconfig -p 2> /dev/null | grep -q 'libwebkit2gtk-4\.1\.so\.0'; then
      say "! Evict needs WebKitGTK 4.1: install libwebkit2gtk-4.1 (webkit2gtk-4.1, webkit2gtk4.1) with your package manager."
    fi ;;
  *) die "unknown package type: $kind" ;;
esac

say "✓ Evict ${version:-} installed. Open it from your applications menu, or run: evict"
