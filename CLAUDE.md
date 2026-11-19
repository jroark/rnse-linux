# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Linux for the Audi RNS-E 192 (SH7750R, variants `V220` Europe and `V222` North America): the kernel's board support and drivers (`overlay/`, `patches/`), user space (`userland/`, `rootfs/`), the build (`build.sh`), and `linuxboot`, the kit program that loads and enters the kernel, from an SD card (started by the kit's SD loader) or installed in flash by `rnse firmware ... --kernel`. It is built on the kit, rnse-sdk (https://github.com/jroark/rnse-sdk; `RNSE_SDK`, default `../rnse-sdk`), and tested on the emulator, rnse (https://github.com/jroark/rnse). It was split out of rnse with its history. `docs/linux.md` says what it does and what has been seen; keep it honest.

Firmware is copyrighted and must never be committed. `.gitignore` blocks `*.iso`, `*.bin`, `*.KWI`, `*.MOT`, `/firmware/`, `/out/`; do not add firmware-derived files (flash dumps, extracted modules, SD card images) to the repository under other names either.
The kernel's source, the toolchain, BusyBox and minimp3 are downloaded, pinned and checked, into `work/` (and a Docker volume); never commit them or `out/`.

## Mandatory hardware behavior

- **Always preserve physical ejection.** Every custom firmware, loader,
  diagnostic and test payload must support opening/folding down the display
  and ejecting the disc. Keep faceplate polling and eject handling responsive
  on normal screens, error screens and during long-running operations. Do not
  repurpose the display/open key without another accessible physical way to
  open the display and reach the disc-eject control. SDK `KEY_DISPLAY` is
  emulator `EJECT`; `KEY_CD_EJECT` is a separate key. Verify the actual eject
  command path before providing hardware test builds, including across handoffs.
- **Hold RETURN for two seconds to return to the loader.** Every payload
  launched by the SD loader must implement this shortcut from every screen,
  close active apps and stop playback before calling `loader_return(status)`.
  A short RETURN retains its normal back/cancel behavior; a long hold must not
  also trigger that short action or immediately relaunch the payload after
  returning. Return directly to the resident loader without resetting or
  invoking an updater. Outside a loader session, report that return is
  unavailable and remain operable, with eject still working.
- **No firmware-flash option unless the user explicitly asks for it.** Default
  custom loaders, test payloads and UIs must neither flash firmware nor expose
  any updater/install/restore/erase option or hidden button sequence that can
  do so. This includes double-SETUP, `stock_update()`, firmware removal and
  automatic updater handoffs. Permission to build, test, recover, return to
  the loader or eject a disc is not permission to flash. Existing updater
  mechanisms described elsewhere in this file are technical background, not
  authorization to expose or invoke them. Include a flash option only when
  explicitly requested by the user.

Hardware incident reported 2026-11-15: double-SETUP on the original loader
invoked an updater and flashed firmware whose eject button did not work. The
disc was trapped and the user had to disassemble the RNS-E. These requirements
apply to all future hardware builds.
## Commands

```sh
./build.sh                                  # out/VMLINUX.BIN, vmlinux (in a container; RNSE_LINUX_NATIVE=1 on a Linux host with sh4-linux-gnu-gcc)
sh ../rnse-sdk/build.sh 192 ./linuxboot     # ../rnse-sdk/out/linuxboot-192.bin
cargo test --release                        # the emulator tests: skip without the variables below
RNSE_DISC=$PWD/disc.iso RNSE_LINUX=$PWD/out/VMLINUX.BIN RNSE_BOOT_ROM=$PWD/rom.bin cargo test --release -- --nocapture
```

`tests/disc.rs` needs `RNSE_DISC` (an update disc image, firmware: never committed), `RNSE_LINUX` (the kernel `build.sh` makes), an SH-4 GCC on the PATH (or the kit's `tools/sh4-docker`), and for the two tests in flash `RNSE_BOOT_ROM` (the file `rnse report --rom` writes; firmware too). Without them the tests return early and report success, which proves nothing. A crate's tests run in its own directory, so give these variables absolute paths.

## Layout

- `build.sh`, `Dockerfile`, `rnse192_defconfig`, `toolchain/` (musl-cross-make), `busybox.config`, `initramfs.list`: the build. It stops when the configuration Kconfig ends up with is not the one written.
- `overlay/` (copied over the kernel's tree) and `patches/` (Kconfig and Makefile entries): the board (`overlay/arch/sh/boards/board-rnse.c`), the controls and the way back (`rnse-controls.c`), the screen and the other drivers.
- `userland/`: `rnse`, one static program (`rnse-gui`, `rnse-radio`, `rnse-play`, `rnse-can`); `rootfs/`: the initramfs's files.
- `linuxboot/`: the second stage, built with the kit.
- `tests/`: the emulator tests (crate `rnse-linux`; `rnse` pinned in `Cargo.toml` and `Cargo.lock`).

A `docs/....md` named in a comment that is not in this repository is in rnse (`docs/hardware.md`, `docs/boot-rom.md`, ...) or, for the kit's own (`docs/custom-firmware.md`, `docs/sd-loader.md`), in rnse-sdk; `sdk/src/...` is the kit's `src/...`.

## Things that are easy to miss

- **Nothing in Linux writes the flash.** Installed in flash, the only way out is the kit's guard (`src/flash.c` in rnse-sdk), which `linuxboot` drives: switching the unit off and on four times, each before the GUI has been up for ten seconds, with the stock disc in the drive, takes Linux out ("Linux in flash" in `docs/linux.md`). Do not add a flash writer or weaken that path.
- **The controls under Linux** are the kernel's (`rnse-controls.c`): EJECT folds the display, CD EJECT pushes the disc out, RETURN held for two seconds goes back to the loader through `linuxboot`'s `loader_return`. Check them on the emulator after any change there.
- The kernel is entered by `linuxboot` with the caches as the kit leaves them; the tests run with the caches modelled and RAM arbitrary, as a unit would.
