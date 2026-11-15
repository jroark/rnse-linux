#!/bin/sh
# Build Linux for the Audi RNS-E 192:   linux/build.sh
#
# The result is linux/out/VMLINUX.BIN, the kernel's image from its zero
# page on (to be loaded at 0x8c001000 and entered at 0x8c002000) with a
# small initramfs built in, and linux/out/vmlinux, the same as an ELF
# file for `rnse boot --gdb`.  sdk/examples/linuxboot, copied to an SD
# card as RNSE.BIN beside VMLINUX.BIN, loads and starts it: see
# docs/linux.md.
#
# The kernel's source is downloaded (pinned, checked against its
# SHA-256) and never kept in the repository.  The build runs in a
# container (the Dockerfile here) with the source in a Docker volume,
# because the kernel's tree does not unpack on a case-insensitive file
# system; set RNSE_LINUX_NATIVE=1 to build directly on a Linux host that
# has sh4-linux-gnu-gcc, make, flex, bison and bc.
set -e
here=$(cd "$(dirname "$0")" && pwd)
version=6.18.63
sha256=81764a545072d160748beff4d9de2e7d95aa9f16e8261110288f067a2cc61d71
url=https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$version.tar.xz

if [ -z "$RNSE_LINUX_INSIDE" ] && [ -z "$RNSE_LINUX_NATIVE" ]; then
    mkdir -p "$here/work" "$here/out"
    tarball=$here/work/linux-$version.tar.xz
    if [ ! -f "$tarball" ]; then
        curl -fL -o "$tarball.part" "$url"
        mv "$tarball.part" "$tarball"
    fi
    docker image inspect rnse-sh4-kernel >/dev/null 2>&1 ||
        docker build -t rnse-sh4-kernel "$here"
    exec docker run --rm -e RNSE_LINUX_INSIDE=1 -e JOBS \
        -v rnse-linux-build:/build -v "$here":/rnse-linux \
        rnse-sh4-kernel sh /rnse-linux/build.sh "$@"
fi

if [ -n "$RNSE_LINUX_INSIDE" ]; then
    src=/rnse-linux
    work=/build
else
    src=$here
    work=$here/work/native
fi
out=$src/out
tarball=$src/work/linux-$version.tar.xz
mkdir -p "$work" "$out"

# Check the download every time: the tree is rebuilt from it when the
# patches change.
echo "$sha256  $tarball" | sha256sum -c - >/dev/null || {
    echo "linux-$version.tar.xz: wrong SHA-256" >&2
    exit 1
}

tree=$work/linux-$version
stamp=$(cat "$src"/patches/*.patch | sha256sum | cut -c1-16)
if [ ! -f "$tree/.rnse-$stamp" ]; then
    rm -rf "$tree"
    tar -xJf "$tarball" -C "$work"
    for p in "$src"/patches/*.patch; do
        patch -d "$tree" -p1 -s < "$p"
    done
    touch "$tree/.rnse-$stamp"
fi
# New files of our own, copied over the tree each time.
cp -R "$src"/overlay/. "$tree"/

make_k() {
    make -C "$tree" O="$work/build" ARCH=sh CROSS_COMPILE=sh4-linux-gnu- \
        -j"${JOBS:-$(nproc)}" "$@"
}

# The configuration: ours, completed by the kernel's defaults.
mkdir -p "$work/build"
sed "s|^CONFIG_INITRAMFS_SOURCE=.*|CONFIG_INITRAMFS_SOURCE=\"$work/init/initramfs.list\"|" \
    "$src/rnse192_defconfig" > "$tree/arch/sh/configs/rnse192_defconfig"
make_k rnse192_defconfig
# Every line of ours must have survived (a misspelt or impossible option
# is dropped by Kconfig without a word).
grep '^CONFIG_' "$tree/arch/sh/configs/rnse192_defconfig" | while read -r line; do
    grep -qxF "$line" "$work/build/.config" || echo "$line"
done > "$work/build/not-taken"
if [ -s "$work/build/not-taken" ]; then
    echo "the configuration was not taken as written; dropped:" >&2
    cat "$work/build/not-taken" >&2
    exit 1
fi

# The kernel's user-space headers, for nolibc.
make_k headers_install INSTALL_HDR_PATH="$work/headers" >/dev/null

# /init, with the kernel's nolibc.
mkdir -p "$work/init"
# (Compiled and linked apart: the driver's -mb asks the linker for an
# emulation Ubuntu's binutils lacks, while `ld -EB` works.)
sh4-linux-gnu-gcc -c -mb -m4 -Os -ffreestanding -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-stack-protector -Wall -nostdinc \
    -isystem "$(sh4-linux-gnu-gcc -print-file-name=include)" -I "$work/headers/include" \
    -include "$tree/tools/include/nolibc/nolibc.h" -I "$tree/tools/include/nolibc" \
    -o "$work/init/init.o" "$src/init/init.c"
sh4-linux-gnu-ld -EB -static -e _start -o "$work/init/init" "$work/init/init.o"
if sh4-linux-gnu-nm "$work/init/init" | grep -q ' U '; then
    echo "init: undefined symbols:" >&2
    sh4-linux-gnu-nm "$work/init/init" | grep ' U ' >&2
    exit 1
fi
sed "s|INIT|$work/init/init|" "$src/initramfs.list" > "$work/init/initramfs.list"

make_k vmlinux
sh4-linux-gnu-objcopy -O binary -R .note -R .comment -S "$work/build/vmlinux" "$out/VMLINUX.BIN"
cp "$work/build/vmlinux" "$out/vmlinux"
cp "$work/build/.config" "$out/config"
cp "$work/build/System.map" "$out/System.map"
ls -l "$out/VMLINUX.BIN"
# The kernel, its bss included, has to end below the SD loader's session
# at 0x8c7c0000, which it leaves alone for the way back.
end=$(sh4-linux-gnu-nm "$out/vmlinux" | awk '$3 == "_end" { print $1 }')
if [ "$((0x$end))" -ge "$((0x8c7c0000))" ]; then
    echo "the kernel ends at 0x$end, over the SD loader's session at 0x8c7c0000" >&2
    exit 1
fi
sh4-linux-gnu-nm "$out/vmlinux" | grep -E ' (_text|_stext|empty_zero_page|_end)$'
