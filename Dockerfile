# What linux/build.sh needs to build the kernel for the RNS-E 192:
# an SH-4 GCC (big-endian code with -mb) and the kernel's host tools.
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-sh4-linux-gnu binutils-sh4-linux-gnu \
        make gcc libc6-dev bc flex bison cpio patch rsync libelf-dev g++ wget curl bzip2 texinfo file xz-utils perl python3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
