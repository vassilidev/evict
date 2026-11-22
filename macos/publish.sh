#!/bin/sh
# Attaches a notarized DMG to its release and makes that release "latest", unless a newer tag exists.
# Usage (CI, with GH_TOKEN and GH_REPO): macos/publish.sh v1.2.0 .build/Evict.dmg
set -eu
tag=$1
gh release create "$tag" "$2" --title "Evict ${tag#v}" --generate-notes --latest=false ||
  gh release upload "$tag" "$2" --clobber
# An older tag finishing late must not take "latest" back
newest=$(gh api "repos/$GH_REPO/tags?per_page=100" --jq '.[].name' | grep '^v' | sort -V | tail -n 1)
if [ "$newest" = "$tag" ]; then gh release edit "$tag" --latest; fi
