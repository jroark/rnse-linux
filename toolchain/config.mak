# musl-cross-make's configuration for a big-endian SH-4 C toolchain with
# musl, for BusyBox (build.sh).  Ubuntu's SH-4 C library is
# little-endian only; the RNS-E's processor runs big-endian.
TARGET = sh4eb-linux-musl
GCC_VER = 13.3.0
BINUTILS_VER = 2.44
MUSL_VER = 1.2.6
COMMON_CONFIG += --disable-nls CFLAGS="-O2" CXXFLAGS="-O2"
GCC_CONFIG += --enable-languages=c --disable-libquadmath --disable-decimal-float \
    --disable-libitm --disable-fixed-point --disable-libsanitizer --disable-libgomp
