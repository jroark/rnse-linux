# Linux on the 192

A Linux kernel for the 192 (North American or European; SH7750R-like
SH-4, 64 MiB SDRAM), started one of two ways:

- from an SD card, through the same chain as any program of the SDK's:
  the stock software starts `sdboot` from the disc, `sdboot` starts
  `RNSE.BIN` from the card, and here `RNSE.BIN` is
  `sdk/examples/linuxboot`, which reads the kernel, `VMLINUX.BIN`, from
  the same card and enters it;
- or installed in the flash in the stock software's place, by an update
  disc that `rnse firmware ... --kernel` makes, and started by the boot
  ROM at every start ("Linux in flash").

It comes up into a plain user interface on the screen, `rnse-gui`, run
with the faceplate's keys and knobs ("The GUI"), with a BusyBox shell
behind it on the second console, typed on with the knob ("Rotary
typing").  It has drivers for the hardware `docs/hardware.md` describes
well enough ("The drivers"): the screen, the faceplate, the display's
motor and the A/D converter, the CD/DVD drive, the SD cards, sound
through a card processor, the radio processor's line, CAN, the flash and
the battery-backed memory.  The root file system is an initramfs built
into the kernel.
The display folds down and the disc comes out at any time, and RETURN
held for two seconds goes back to the SD loader when there is one
(CLAUDE.md, "Mandatory hardware behavior"): see "The controls, and the
way back".

What has been seen so far is in the emulator only (with a unit's boot
ROM for the flash); see the end.

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

User space wants a C library for big-endian SH-4, which Ubuntu does not
have (its SH-4 one is little-endian).  So the build also makes a
`sh4eb-linux-musl` toolchain with musl-cross-make (pinned by commit and
checked; it fetches GCC 13.3.0, binutils 2.44 and musl 1.2.6 itself and
checks each), once, into the same volume (`linux/toolchain/`; it takes
a while the first time), and with it a static BusyBox 1.37.0 (pinned,
checked; `linux/busybox.config` has our changes to its defconfig) and
our own `linux/userland`, with minimp3 for MP3 (one header, CC0, pinned
by commit and checked against its SHA-256 like the rest).

The build stops when the configuration Kconfig ends up with is not the
one written: an option that is misspelt or not possible is otherwise
dropped without a word.

What is ours, besides the configuration:

| file | what |
|------|------|
| `linux/overlay/arch/sh/boards/board-rnse.c` | the board: machine vector, the watchdog's strobe, the companion chip's interrupt sources (an interrupt controller of their own), the platform devices, the flash's partitions, the battery-backed memory, the loader's memory kept |
| `linux/overlay/arch/sh/boards/rnse-controls.c` | the faceplate's keys and knobs (also an input device), the display's motor, the A/D converter, the disc's eject, the way back to the loader, `/sys/kernel/rnse` |
| `linux/overlay/drivers/video/fbdev/rnse-coralfb.c` | the screen: one layer of the MB86293, 400x235 at 8 bits through its palette |
| `linux/overlay/drivers/...`, `linux/overlay/sound/...` | the other drivers: "The drivers" |
| `linux/overlay/include/linux/platform_data/rnse.h` | what the drivers share: the interrupt sources, the converter, the eject |
| `linux/patches/0001-sh-rnse-192-board.patch` | Kconfig and Makefile entries for the board, the controls and the screen, the board's machine type, and the processor's revision (below) |
| `linux/patches/0002-rnse-192-drivers.patch` | Kconfig and Makefile entries for the other drivers |
| `linux/userland/` | `rnse`, one static program with four names: `rnse-gui`, `rnse-radio`, `rnse-play`, `rnse-can` |
| `linux/initramfs.list`, `linux/rootfs/` | the initramfs: BusyBox's init as `/init`, `/etc/inittab` (the GUI on the screen's first console, `tty1`, a shell on the second and one on the serial port), `/etc/init.d/rcS`, `/etc/profile` |
| `linux/toolchain/`, `linux/busybox.config` | the user-space toolchain and BusyBox's configuration |
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

    PATH=$PWD/sdk/tools/sh4-docker:$PATH RNSE_DISC=stock.iso RNSE_LINUX=linux/out/VMLINUX.BIN \
        cargo test --release --test disc linux_boots_from_an_sd_card

It runs with the caches modelled and RAM full of arbitrary data, and
checks the kernel's messages, BusyBox's start, a command typed on the
screen's shell with the knob, the screen, the watchdog's strobe,
the controls while the card is read and under Linux, the way back, and
that nothing was fetched stale or written back lost.

On a unit: `RNSE.BIN` and `VMLINUX.BIN` on a FAT card as for any program
from a card ("Programs from an SD card" in
[custom-firmware.md](custom-firmware.md)), the card in before the
`sdboot` disc.  `VMLINUX.BIN` is 3.1 MB, the kernel with BusyBox in an
xz-compressed initramfs; at the card processor's pace (293 KB/s measured
through the stock program) that is some 11 s to read (6 s in the
emulator).  The kernel then takes about 1.2 s to unpack the initramfs
(interrupts open, the watchdog served) and the shell is there.

A card may also hold `AUTORUN.SH`: `/etc/init.d/rcS` mounts the card
and runs it in the background once the card is there, output on the
console.  That is how the end-to-end tests drive Linux, and handy on a
bench; it is no more trust than the SD loader gives `RNSE.BIN`.

## The drivers

Each is the SDK's driver of the same device, or what `docs/hardware.md`
says of it, in the kernel.  The companion chip's interrupt sources each
get a level of their own (the stock software's), and so an IRQ of their
own: the SH-4 sees the encoded level as INTEVT 0x200 + (15 - level) *
0x20, which Linux numbers INTEVT >> 5, 31 - level; masking an IRQ gives
its source level 0 (`board-rnse.c`).

| device | driver | Linux sees |
|--------|--------|-----------|
| screen (Coral, layer L4) | `drivers/video/fbdev/rnse-coralfb.c` | `/dev/fb0`, 400x235, 8 bits, palette |
| faceplate, display's motor, CD EJECT, RETURN held | `arch/sh/boards/rnse-controls.c` | two input devices; `/sys/kernel/rnse/{mode,healthy,display,eject}` |
| A/D converter, seven inputs | `drivers/iio/adc/rnse-adc.c` (through `rnse_adc_read`, the controls' lock) | `/sys/bus/iio/devices/iio:device0/in_voltageN_raw` |
| CD/DVD drive (ATAPI) | `drivers/ata/pata_rnse.c`: libata, programmed I/O, INTRQ at level 3 | `/dev/sr0`; ISO 9660 and UDF |
| SD cards, both slots | `drivers/misc/rnse-media.c`: the stock program for the card processors from the flash (as `sdk/src/sd.c`), a card taken as the loader left it or started anew, read only | `/dev/rnsesd0`, `/dev/rnsesd1` and their partitions; VFAT |
| sound | `drivers/misc/rnse-media.c`: our own program in a card processor (as `sdk/src/pcm.c`), fed by DMA channel 7 or 3, the end of a pass through the buffer an interrupt (the DMA controller's eight all arrive as the first's, IRQ 50) | ALSA card 0, `RNSE`: 44.1 kHz, stereo, 16 bits big-endian; the "PCM Processor" control |
| radio processor (tuner, amplifier) | `drivers/tty/serial/rnse-uart.c`: companion serial channel 1, level 2; the protocol in user space (`rnse-radio`), but for the keep-alive, 81 [06] once a minute, which the controls thread sends through the driver between user space's frames (`rnse_radio_send`; without it the radio processor restarts the unit, `docs/hardware.md`) | `/dev/ttyRN1`, 9600 bit/s, 8N1 |
| CAN | `drivers/net/can/rnse-hcan2.c`: the HCAN2, mailbox 0 receives, 1 sends, level 5; a frame nobody acknowledges is given up after 100 ms | `can0`, SocketCAN, 100 kbit/s by default |
| flash, 32 MiB | physmap as a ROM (`map_rom`): no command ever goes to the chips, nothing can be written | `/dev/mtd0`..`6`: bootrom, directory, os, apm, aum, font, rest |
| battery-backed memory, 256 KiB | `board-rnse.c`, an nvmem device | `/sys/bus/nvmem/devices/rnse-sram/nvmem`; only 0x3f640..0x3f7ff can be written (Linux's settings) |

Not here: GPS (the baseband on chip select 5 is not known well enough:
`docs/hardware.md`, "GPS"), the SH-4's clock (it does not run on a
unit), writing to cards (how the card processors' program writes is not
known), and anything that writes the flash.

Sound and the cards share the processors: one that plays has no card.
Sound takes the processor of a slot without a disk, processor 0 first
(or the one the "PCM Processor" control names), else one whose disk
nobody has open, which is then removed; when the sound is closed the
card is started anew.  Which processor's converter reaches the
amplifier on a unit is still to be heard (`sdk/examples/pcmtest`); the
amplifier's source is the radio processor's to set (`rnse-radio source
decoder`).

The user-space side, `linux/userland`, one static program `rnse`:

    rnse-radio start | status | version | source tuner|decoder|cd [VOLUME] | volume N | tune MHZ
    rnse-play FILE.wav|FILE.mp3 ...       # WAV (16 bits) and MP3, any rate, to 44.1 kHz
    rnse-can up | send ID#HEX ... | dump [N]

MP3 is decoded on the SH-4 (minimp3); in the emulator, one instruction
a cycle, it keeps up with the converter, at 44.2 thousand frames a
second.  ALSA is spoken through its ioctls, as tinyalsa does: there is no
alsa-lib.

## The GUI

`rnse-gui` runs on the first console (`/etc/inittab`, respawned): the
framebuffer at 8 bits through its palette, drawn in RAM and copied a
word at a time with its bytes turned round (the Coral chip's pixel N is
the byte at N ^ 3), letters from the console's own font (`KDFONTOP`),
the keys from the "RNS-E faceplate" input device; it grabs the rotary
keyboard so that nothing is typed behind it.

| control | does |
|---------|------|
| select knob | up and down a list; on the radio, 0.1 MHz |
| SELECT | open, play, do |
| RETURN (short) | back; a long one is the kernel's (the loader) and never reaches the GUI |
| volume knob | the amplifier's volume (0 to 30), kept in the battery-backed memory |
| RADIO, MEDIA, CAR, SETUP, INFO | the radio, the media, CAN, the system, the start page |
| NEXT, PREV | the next or previous track; on the radio, 1 MHz |
| top left soft key | on Media: stop; on CAN: send 7F0 `52 4E 53 45` |
| EJECT, CD EJECT | the kernel's: the display, the disc |

The pages: Radio (FM, the frequency, the station's text from the radio
processor's status), Media (the cards and the disc, mounted read only;
WAV and MP3, one file after the other, through `rnse-play`), CD (mount,
unmount, eject), CAN (the last frames, counts, a test frame), System
(how the kernel was started, the guard's count, the display's position,
the A/D inputs, the cards, the radio processor; the display, the eject,
the shell).  "Shell" goes to the second console, where the BusyBox
shell is; INFO comes back.

Started from flash, the GUI writes `/sys/kernel/rnse/healthy` once it
has drawn its first picture and run for ten seconds: the guard's count
goes back to nothing (below).

## Linux in flash

    sdk/build.sh 192 linuxboot && sdk/build.sh 192 swap
    rnse firmware stock.iso V222 sdk/out/linuxboot-192.bin linux-flash.iso \
        --kernel linux/out/VMLINUX.BIN --swap sdk/out/swap-192.bin --index NNNN

The disc is an update disc as any `rnse firmware` makes
(`docs/custom-firmware.md`, "A program in flash"), with one module more:

- the operating system's place (flash 0xc100) holds `linuxboot`, with
  the start-up instructions that copy it to RAM and enter it (and pass
  it the mark of a program started from flash);
- the application's place (APM, flash 0x210000, 0x80ec28 bytes) holds
  the kernel (`fw::kernel_module`): the stock module's header, load
  address and length, its three entry points on an `rts` of its own,
  then at +0x100 `RNSELNX1`, the kernel's length and its CRC-32, and the
  kernel image at +0x200, the rest 0xff.  The boot ROM checks the
  operating system's sum only and calls that module's first entry
  first, which for a program never returns;
- the card processors' program stays where it was, at 0x200000 in the
  operating system's module, which `firmware_module` keeps from its
  second megabyte on: the SD driver needs it;
- `--swap` is required: a unit whose Linux was taken out must not
  install it again from this disc.

At every start the boot ROM starts `linuxboot`; its start-up code counts
the start in the battery-backed memory (the guard, `sdk/src/flash.c`).
It starts the faceplate controller from nothing (the boot ROM leaves it
in reset), copies the kernel from the flash to 0x8c001000 through
uncached addresses with the watchdog strobed and the controls answered,
checks its CRC, and enters it with `rnse.flash=1` on its command line
instead of `rnse.return=`.  The kernel then says the way back to a
loader is not available, starts the cards from nothing, and the GUI
starts the radio processor as the stock software would
(`radio_start`'s frames).  A kernel that is not there or not whole is
said on the screen and nothing is started; the controls go on working.

**The way out is the guard, and only the guard.**  Nothing in Linux,
`linuxboot` or the GUI erases or writes the flash (the kernel's flash
driver is a ROM driver).  A start counts as one that worked only when
the GUI has been up for ten seconds; at the fifth start in a row that
did not, the guard erases the directory sector and restarts, and the
boot ROM installs the update disc in the drive.  So, to take Linux out:
the stock update disc in the drive, then off and on four times, each
time within ten seconds of the GUI's picture.  A Linux that does not
get as far as the GUI (a kernel that is not whole, one that panics) is
taken out the same way, by itself, since the watchdog or the person
restarts it.

In the emulator with a unit's boot ROM, `--strict`, all of it
(`tests/disc.rs`: `linux_is_installed_in_flash`,
`linux_in_flash_with_a_spoilt_kernel_is_taken_out_by_the_guard`): the
install through the stock software and its loader, the ROM starting
`linuxboot`, the kernel from the flash, the GUI, the start counted as
healthy 12.5 s after the reset, the controls, a power cycle back into
Linux, and with a spoilt kernel the guard taking it out and the ROM
putting the stock software back, byte for byte.  (The test's "power
cycle" resets the processor only, so it also puts the drive in the state
of a power-on with the stock disc in.  A disc changed while the unit ran
and then only the processor reset left the emulated drive with its
"medium changed" still to report, and the boot ROM did not get past
that.  A power cut resets the drive on a unit, so the way out is: the
stock disc in, then off and on; but whether a unit's ROM minds a drive
that reports a power-on reset first has not been seen.)

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
reserves.

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
  anew or RETURN is pressed again.  A shorter RETURN is Backspace
  (below), sent when it is let go; a long one sends nothing.  Started
  any other way, the kernel says that the way back is not available
  and carries on.

## Rotary typing

The faceplate has no keyboard, so the kernel's controls driver types
with what it has, on the shell of the second console (the GUI's
"Shell"; INFO goes back to the GUI).  The select knob runs through a ring of characters,
and the character it stands on is typed at once and, while the knob
turns, replaced (Backspace and the next one), so the shell shows it in
place and needs to know nothing of this:

| control | does |
|---------|------|
| select knob | the next or the previous character; the first detent after a kept one types the character the ring stands on (the last one kept, `a` at first) |
| pressing it (SELECT) | keeps the character; with none being chosen, Enter |
| NEXT, PREV | the start of the next or the previous group: `a`-`z`, `0`-`9`, the symbols ``-./_\|>&;=*~$'"<()[]{}!?#@%+:,\^` ``, `A`-`Z` |
| RETURN (short) | Backspace |
| soft keys | top left Tab, top right Ctrl-C, bottom left Space, bottom right Enter |
| volume knob | Up and Down, the shell's history |
| EJECT, CD EJECT, RETURN held | as above: the display, the disc, the way back |

The typed keys come from an input device of their own, "RNS-E rotary
keyboard", which the console's keyboard handler takes as it would a
keyboard (US layout).  The keys as they are, for programs of one's own,
are another, "RNS-E faceplate": `BTN_TRIGGER_HAPPY1` and on in the order
of `keymap[]` in `rnse-controls.c` (INFO, NAV, TEL, NAME, MEDIA, RADIO,
the four soft keys, SETUP, CAR, NEXT, PREV, SELECT, RETURN, EJECT, CD
EJECT), codes the console leaves alone, and the knobs as `REL_DIAL`
(select) and `REL_MISC` (volume).  RETURN there is pressed and let go
together when a short one is let go, and not at all for a long one, so
that no program takes the way back for "back".

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
64 MiB, its timers and serial ports, and comes up into the GUI with the
watchdog served throughout (the longest pause 16 ms); a command typed
with the knob in the shell behind it; the display folding down and up,
the disc coming out (also while sound plays), RETURN held bringing
`sdboot` back (`linux_boots_from_an_sd_card`).  Every driver from user
space (`linux_drivers_and_gui_from_an_sd_card`): the flash and the
battery-backed memory read, the A/D converter, the card mounted, the
radio processor answering, a CAN frame out on the bus and the bus's
frames in, the disc mounted and read by programmed I/O, a WAV and an MP3
file played through a card processor's converter at 44.1 kHz in real
time, and the GUI's pages by their keys.  And under a unit's boot ROM,
Linux installed in flash by the stock software's own update, started at
every start, counted healthy, and taken out by the guard when its kernel
is spoilt ("Linux in flash").  All with the caches modelled and RAM
arbitrary, at one instruction a cycle.

Not seen on a unit yet.  What the emulator cannot vouch for there:

- the 105 ms of the watchdog against the time the kernel really takes
  from its entry to its strobe timer, and against linuxboot's copy of
  4.5 MB from the flash (strobed every 64 KiB): the emulator has no
  times for the bus, and the unit runs memory-heavy code at about half
  the emulator's pace;
- the companion chip's interrupt levels for the drive (3), the radio
  processor's line (2) and CAN (5) are the stock software's, but only
  CAN's has been used by a program of ours on a unit;
- which card processor's converter reaches the amplifier
  (`sdk/examples/pcmtest` asks; the "PCM Processor" control chooses);
- the DMA controller pacing sound by the processor's pin: the emulator
  runs the processor ahead in bursts; a unit may want a deeper buffer;
- MP3 on the SH-4 at a unit's real pace (the emulator's one instruction
  a cycle kept up, with no margin measured);
- the drive by programmed I/O under libata (the stock software uses DMA
  for data; the SDK's PIO reads have run in the emulator only);
- the controls' timing against a real faceplate controller, motor and
  drive, and the way back on real caches (two-way, the model
  direct-mapped);
- anything of the SH-4 that the stock software never used and the
  emulator models from the manual alone: the MMU and its TLB misses,
  the second timer unit (0xfe100000), the SCIF.

Not done: GPS, writing to cards, the card processors' own MP3 decoder,
a compressed kernel (4.5 MB from a card is some 15 s at the card
processor's pace), and settings beyond the radio's in the battery-backed
memory.
