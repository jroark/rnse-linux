# Linux on the 192, from an SD card

A Linux kernel for the 192 (North American or European; SH7750R-like
SH-4, 64 MiB SDRAM), started from an SD card through the same chain as
any program of the SDK's: the stock software starts `sdboot` from the
disc, `sdboot` starts `RNSE.BIN` from the card, and here `RNSE.BIN` is
`sdk/examples/linuxboot`, which reads the kernel, `VMLINUX.BIN`, from the
same card and enters it.  The root file system is a small initramfs
built into the kernel; Linux has no driver for the cards themselves.
The display folds down and the disc comes out at any time, and RETURN
held for two seconds goes back to the SD loader (CLAUDE.md, "Mandatory
hardware behavior"): see "The controls, and the way back".

What has been seen so far is in the emulator only; see the end.

## Building

    linux/build.sh                       # linux/out/VMLINUX.BIN, vmlinux, config, System.map
    sdk/build.sh 192 linuxboot           # sdk/out/linuxboot-192.bin

`linux/build.sh` downloads the kernel's source (6.18.63, pinned and
checked against its SHA-256) into `linux/work/`, applies
`linux/patches/`, copies `linux/overlay/` over the tree, and builds with
`linux/rnse192_defconfig` in a container (`linux/Dockerfile`: Ubuntu's
`gcc-sh4-linux-gnu`, which makes big-endian code with `-mb`).  The tree
lives in a Docker volume, `rnse-linux-build`, because it does not unpack
on a case-insensitive file system; `RNSE_LINUX_NATIVE=1` builds on a
Linux host directly.  A build from nothing takes a few minutes, after
that well under one.  No kernel source is kept in the repository.

The build stops when the configuration Kconfig ends up with is not the
one written: an option that is misspelt or not possible is otherwise
dropped without a word.

What is ours, besides the configuration:

| file | what |
|------|------|
| `linux/overlay/arch/sh/boards/board-rnse.c` | the board: machine vector, the watchdog's strobe, the companion chip's interrupt sources masked, the graphics chip as a platform device, the loader's memory kept |
| `linux/overlay/arch/sh/boards/rnse-controls.c` | the faceplate's keys and knobs (also an input device), the display's motor, the disc's eject, the way back to the loader |
| `linux/overlay/drivers/video/fbdev/rnse-coralfb.c` | the screen: one layer of the MB86293, 400x235 at 8 bits through its palette |
| `linux/patches/0001-sh-rnse-192-board.patch` | Kconfig and Makefile entries for the two, the board's machine type, and the processor's revision (below) |
| `linux/init/init.c`, `linux/initramfs.list` | `/init`, built with the kernel's own nolibc |
| `sdk/examples/linuxboot` | the second stage on the card |

## Running it

In the emulator, with the stock software doing the starting as on a
unit:

    sdk/build.sh 192 sdboot
    rnse custom stock.iso V222 sdk/out/sdboot-192.bin sdboot.iso --index 0380
    mkdir card && cp sdk/out/linuxboot-192.bin card/RNSE.BIN && cp linux/out/VMLINUX.BIN card/
    rnse card card.img card
    rnse boot sdboot.iso --variant V222 --ignition --installed-index 0370 \
        --sd-card card.img --cycles 9000000000 --screenshot linux.png

The kernel's console comes out on the SH-4's own serial port (the
emulator prints it) and on the screen.  `rnse boot --gdb PORT` with
`linux/out/vmlinux` debugs the kernel.  `--key EJECT@MS`,
`--key CD_EJECT@MS` and `--key RETURN@MS+2500` try the controls.  The
end-to-end test:

    PATH=$PWD/tools/sh4-docker:$PATH RNSE_DISC=stock.iso RNSE_LINUX=linux/out/VMLINUX.BIN \
        cargo test --release --test disc linux_boots_from_an_sd_card

It runs with the caches modelled and RAM full of arbitrary data, and
checks the kernel's messages, `/init`, the screen, the watchdog's strobe,
the controls while the card is read and under Linux, the way back, and
that nothing was fetched stale or written back lost.

On a unit: `RNSE.BIN` and `VMLINUX.BIN` on a FAT card as for any program
from a card ("Programs from an SD card" in
[custom-firmware.md](custom-firmware.md)), the card in before the
`sdboot` disc.  At the card processor's pace (293 KB/s measured through
the stock program) the 2.5 MB take some 8.5 s to read.

## The hand-over

`linuxboot` loads the image to 0x8c001000, where it begins with the
kernel's zero page; the kernel, bss and all, has to end below 0x8c7c0000,
where `sdboot` keeps its session and its saved copy (`linux/build.sh`
checks).  Before it jumps to 0x8c002000 (`_stext`) it writes the way
back into the zero page's command line (`rnse.return=ADDR,STACK`: its
own `loader_return` and stack, when it was started by `sdboot`; the
kernel appends its built-in line, `CONFIG_CMDLINE_EXTEND`), masks
interrupts, stops the timers and every DMA channel, masks the on-chip
interrupt sources and the companion chip's (levels at 0x07b00000 to 0,
enables at +0x1e to 0), writes the operand cache back over the image and
the room for its bss (`ocbp`), strobes the watchdog once more, and
invalidates the instruction cache (CCR.ICI) from an uncached address,
eight instructions before the branch as the SH-4 manual wants.  The
stock operating system ran from the same addresses, and its code would
still be in the instruction cache.

The memory is Linux's afterwards, 64 MiB from 0x0c000000, except what
the way back needs: `sdboot`'s session and copy (0x0c7c0000, 256 KiB)
and `linuxboot` itself (0x0df00000, 128 KiB), which the board code
reserves.  The kernel takes 3 MiB and leaves 59 MiB free at `/init`.

## The controls, and the way back

Whatever runs, the display has to fold down and the disc come out, and
a program the SD loader started has to go back to it on RETURN held for
two seconds.  `linuxboot` does this while it reads the card (from
`sd_idle`, which `sd_load` now calls after every sector: a read need
not wait for anything, and before that a 2.5 MB file was seconds of
not answering the faceplate), and under Linux a kernel thread does it
every 10 ms (`rnse-controls.c`), so that it does not depend on user
space:

- it answers the faceplate controller as the loader left it, started
  and lit (`panel_attach` in the SDK, the same in the kernel), and gives
  it its settings again should it announce itself anew;
- EJECT (the faceplate's key, the SDK's `KEY_DISPLAY`) runs the display's
  motor towards open or closed (port 3: bit 3 open, bit 4 closed), and
  stops it as the SDK's `sdk/src/display.c` and the stock software do:
  once the position (A/D input 3) has stood still for 300 ms, and after
  8 s in any case; EJECT while it runs stops it.  `linuxboot` uses the
  SDK's own (`panel_service`);
- CD EJECT (behind the display) sends the drive START STOP UNIT with
  LoEj, as `sdk/examples/base` does, after folding the display down if
  it is not (open: at most 94, or the switch on port 3 bit 2 low);
- RETURN held for 2 s returns to `sdboot`: the kernel stops its timers
  (both units), DMA and every interrupt source, writes the caches back,
  and from an uncached address purges the operand cache over all of
  RAM, switches the MMU off, sets CCR as the stock software has it and
  calls `linuxboot`'s `loader_return` on `linuxboot`'s stack, which
  puts `sdboot` back and starts it.  `sdboot` shows "TEST RETURNED: 0"
  and waits; it does not start Linux again until the card goes in
  anew or RETURN is pressed again.  A shorter RETURN goes to user space
  as `KEY_BACK` when it is let go; a long one sends nothing.  Started
  any other way, the kernel says that the way back is not available
  and carries on.

The keys and knobs are also an input device ("RNS-E faceplate",
`/dev/input/event0`): the soft keys as F1-F4, SELECT as Enter, the
select knob as `REL_DIAL` and the volume knob as `REL_MISC`.

## What the 192 wanted of Linux

- **The watchdog.**  The board's external watchdog resets the unit 105
  to 115 ms after the last edge on its strobe (port 0x12300001 bit 0,
  measured; `WATCHDOG_MS` in `src/machine.rs`).  The board code toggles
  it once in its setup and then every 10 ms from an hrtimer started at
  `early_initcall`.  The gap between the two, the kernel's own start, is
  the danger: anything slow before the timers run resets the unit.
  `earlycon` on the serial port is: it writes the early messages at
  115200 bit/s with nothing else running, and the emulator's watchdog
  reset the unit in the middle of them.  So no `earlycon`, and
  `lpj=` preset so that the delay loop is not calibrated either.  With
  that, the longest pause the emulator sees from the stock software's
  start to `/init`'s rounds is 16 to 33 ms.  The test fails above 60.
- **The processor's revision.**  The unit's SH-4 says PVR 0x04050086
  and PRR 0x00000200.  Linux knows the SH7750R by PRR 0x10, takes this
  for an unknown SH-4 and leaves the type unset.  The patch adds 0x20.
  The caches it then reads from CVR (16 KiB instruction, 32 KiB
  operand, both two-way) are the SH7750R's.
- **The peripheral clock** is 58.8 MHz (`CONFIG_SH_PCLK_FREQ`), which
  the timers, the serial port's bit rate and the tick depend on.  The
  emulator's processor runs at four times that, and the kernel's preset
  `lpj` is for 235 MHz.
- **The external interrupts.**  The companion chip drives the SH-4's
  IRL lines for the drive, the CAN controller, the card processors and
  more, and an IRL request can only be held off by the processor's
  interrupt mask.  Linux runs with that mask open, so a request nobody
  takes is an endless stream of interrupts.  The board code gives every
  source level 0.  A driver for one of them has to give its source a
  level and clear its latch, as `docs/hardware.md` ("Interrupts")
  describes.
- **The screen** is the graphics chip's layer L4 as the SDK sets it up
  (`board_init` in `sdk/src/hw192.c`, which `linuxboot` runs).  The chip
  is little-endian on a big-endian bus without the byte lanes swapped,
  so pixel N is the byte at N ^ 3.  To the kernel that is a
  little-endian framebuffer on a big-endian processor, which its own
  drawing code handles (`CONFIG_FB_FOREIGN_ENDIAN`,
  `FBINFO_FOREIGN_ENDIAN`).  The driver only sets the palette.  The
  console is 50 columns by 29 lines with the 8x8 font.
- **The screen's light** is the faceplate controller's, and started
  anew that controller wants to be talked to until it has its settings.
  `linuxboot` and the kernel take it as `sdboot` left it, lit, and go on
  answering it; neither resets it.

## What it is and is not, so far

Seen in the emulator: from the stock software's start through `sdboot`
and `linuxboot` to the kernel, which finds an SH7750R with its caches,
64 MiB, its timers and serial ports, puts its console on the screen,
runs `/init` and counts uptime, with the watchdog served throughout;
the display folds down and up, the disc comes out, and RETURN held
brings `sdboot` back.  This was seen at one instruction a cycle
(`--cpu-ratio 4`) and in the test also with the caches modelled.

Not seen on a unit yet.  What the emulator cannot vouch for there:

- the 105 ms of the watchdog against the time the kernel really takes
  from its entry to its strobe timer.  In the emulator it is a few
  milliseconds of instructions, but the emulator has no times for the
  bus, and the unit runs memory-heavy code at about half the
  emulator's pace;
- the controls' timing against a real faceplate controller, motor and
  drive: the emulator's motor sweeps in 1.5 s and its drive ejects at
  once; the unit's may take longer (`sdk/examples/suite`, test 19, times
  the motor);
- the way back on real caches: the emulator's cache model is
  direct-mapped, the SH7750R's two-way;
- anything of the SH-4 that the stock software never used and the
  emulator models from the manual alone: the MMU and its TLB misses,
  the second timer unit (0xfe100000), the SCIF.

Not done: CAN, sound (radio processor and
card processors), the cards themselves, the companion chip's interrupts,
a user space beyond `/init` (Ubuntu's SH-4 C library is little-endian,
so BusyBox would need a big-endian toolchain such as Buildroot's), and
a compressed kernel to cut the card's 8.5 s.
