#!/bin/sh
# Build the sh4eb-linux-musl toolchain into $1 (once): called by
# linux/build.sh inside its container.  musl-cross-make is pinned by
# commit and checked against its SHA-256; it fetches GCC, binutils, musl
# and their libraries itself and checks each against its own hashes.
set -e
prefix=${1:?prefix}
here=$(cd "$(dirname "$0")" && pwd)
commit=5e8042e63de29f8473fc3b0b831a587ba284e1a8
sha256=c4553b236d4acb4e7990860597421f51bfb6ba2a7942f97388af335774397b7e
dl=${RNSE_DL:?download directory}
stamp="$prefix/.rnse-$(cat "$here/config.mak" | sha256sum | cut -c1-16)"
[ -f "$stamp" ] && exit 0
tarball=$dl/musl-cross-make-$commit.tar.gz
[ -f "$tarball" ] || curl -fsSL -o "$tarball" "https://github.com/richfelker/musl-cross-make/archive/$commit.tar.gz"
echo "$sha256  $tarball" | sha256sum -c - >/dev/null
work=$(dirname "$prefix")/mcm
rm -rf "$work" "$prefix"
mkdir -p "$work"
tar -xzf "$tarball" -C "$work" --strip-components=1
cp "$here/config.mak" "$work/config.mak"
# (Its own downloads go to its default place: given another, it takes
# the sources for already fetched.)
make -C "$work" -j"${JOBS:-$(nproc)}"
make -C "$work" OUTPUT="$prefix" install
rm -rf "$work"
touch "$stamp"
