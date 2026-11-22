#!/bin/sh
# Builds the Linux release files in .build/ inside an Ubuntu 20.04 container (glibc 2.31, the oldest Evict supports), so
# the binaries run on every newer distribution. Its WebKitGTK 4.0 headers serve 4.1 too: the app loads either at startup. Works from macOS or Linux with Docker; CI runs it on x86_64 and ARM64.
# PLATFORM=linux/amd64 builds for another architecture (emulated, slower).
set -e
cd "$(dirname "$0")/.."
mkdir -p .build
python3 minify.py index.html .build/index.min.html 2> /dev/null || rm -f .build/index.min.html
docker run --rm ${PLATFORM:+--platform "$PLATFORM"} -v "$PWD:/src" -w /src ubuntu:20.04 sh -ec '
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends gcc libc6-dev pkg-config libwebkit2gtk-4.0-dev curl ca-certificates > /dev/null
  arch=$(dpkg --print-architecture)
  case $arch in
    amd64) sum=4a4402f8f9ea87c669d6a62b333f8e268439974754dd886a01731ecaf9fa6d66 ;;
    arm64) sum=2529b6b3fbd107b57064158a183de6a9801f78d5232980cf46419564491fa5c2 ;;
  esac
  curl -fsSL -o /tmp/nfpm.deb "https://github.com/goreleaser/nfpm/releases/download/v2.43.0/nfpm_2.43.0_$arch.deb"
  echo "$sum  /tmp/nfpm.deb" | sha256sum -c - > /dev/null
  dpkg -i /tmp/nfpm.deb > /dev/null
  linux/build.sh package'
