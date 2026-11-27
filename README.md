# rnse-linux

The Audi RNS-E 192 (8P0 035 192) runs a 6.18 kernel with board
support and drivers for the screen, the faceplate, the fold-down display,
CAN, the CD drive, the SD cards and sound (through the card processors),
the radio processor's line, the A/D converter, flash (read only) and the
battery-backed memory; BusyBox; and a GUI run with the faceplate's keys
and knobs, with a shell behind it typed on with the knob. It starts from
an SD card through the SD loader of
[rnse-sdk](https://github.com/jroark/rnse-sdk), or from flash.
[docs/linux.md](docs/linux.md) says what it does and what has been seen.

It is tested on [rnse](https://github.com/jroark/rnse), the emulator it
was split out of with its history (at rnse commit 0e3a2d4ce5941161e17ff4816126281fbcf80088). What has
been seen so far was seen in the emulator only.

```sh
git clone https://github.com/jroark/rnse-sdk ../rnse-sdk
./build.sh                                  # out/VMLINUX.BIN (downloads pinned; builds in Docker)
sh ../rnse-sdk/build.sh 192 ./linuxboot     # ../rnse-sdk/out/linuxboot-192.bin
RNSE_DISC=$PWD/disc.iso RNSE_LINUX=$PWD/out/VMLINUX.BIN cargo test --release -- --nocapture
```

The RNS-E 193 build boots into the faceplate GUI from SD. Its board
drivers support both SD slots, the optical drive, CAN, the radio and
stereo playback. To reproduce the emulator run, use a stock firmware
ISO containing the 193 variant (tested with V575), the emulator revision
in Cargo.lock and an SDK with the 193 loader return path (tested with
`2a0ba8e`). Set `RNSE_SDK` and `RNSE_EMU` if the checkouts are outside
the default sibling directories `../rnse-sdk` and `../rnse`.

```sh
RNSE_UNIT=193 ./build.sh
RNSE_DISC=/absolute/path/to/firmware.iso tools/test-193.sh
```

No kernel source, firmware or data is included: the kernel, the
toolchain, BusyBox and minimp3 are downloaded, pinned, by `build.sh`.
Your unit, your discs and your data are your own risk. The display keeps
opening and the disc ejecting under Linux, RETURN held for two seconds
goes back to the SD loader, and nothing in Linux writes the flash: from
flash the only way out is the kit's guard. See CLAUDE.md, "Mandatory
hardware behavior", and "Linux in flash" in docs/linux.md before
installing it in flash.

GPL-2.0-only ([LICENSE](LICENSE)).
