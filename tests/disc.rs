//! Linux's end-to-end tests: the kernel (`build.sh`, `RNSE_LINUX`)
//! started by `linuxboot` the way a unit starts it, from an SD card
//! through the SD loader and installed in flash under a unit's boot ROM,
//! and run on the emulator.
//!
//!     RNSE_DISC=disc.iso RNSE_LINUX=out/VMLINUX.BIN cargo test --release -p rnse-linux -- --nocapture
//!
//! They need `RNSE_DISC`, an image of an update CD (firmware, never in a
//! repository), `RNSE_LINUX`, the kernel as `build.sh` makes it, an SH-4
//! GCC on the PATH (the kit's `tools/sh4-docker` has one in a
//! container), and for the two in flash `RNSE_BOOT_ROM`, the file
//! `rnse report --rom` writes (firmware too).  Without them they return
//! early and pass, which proves nothing.  The kit is `RNSE_SDK`, or
//! rnse's `sdk/` beside this directory.  The helpers are copies of
//! rnse's `tests/disc.rs`, taking a program of this repository by path.

use rnse::fw;
use rnse::machine::Machine;
use rnse::sh4::cpu::Stop;

/// The kit: RNSE_SDK, or rnse's sdk/ beside this directory.
fn kit() -> std::path::PathBuf {
    std::env::var_os("RNSE_SDK").map(std::path::PathBuf::from)
        .unwrap_or_else(|| std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../sdk"))
}

/// A program of this repository, as build.sh takes it (a path).
fn own(program: &str) -> String {
    std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join(program).to_string_lossy().into_owned()
}

/// What build.sh names a program's output after: its last component.
fn output_name(example: &str) -> &str {
    example.rsplit('/').next().unwrap()
}

fn disc() -> Option<Vec<u8>> {
    let path = std::env::var_os("RNSE_DISC")?;
    Some(std::fs::read(path).expect("RNSE_DISC is not readable"))
}

/// A program written from scratch (sdk/examples/hello), built with an
/// SH-4 GCC if there is one, put in the loader's place on the disc and
/// started the way the boot ROM starts the loader: it brings the board
/// up, prints on the debug serial port, draws and keeps time, on both
/// units.
/// Build an SDK example for a unit, put it in the loader's place and
/// start it.  None when there is no cross compiler or the disc has no
/// software for that unit.
fn sdk_example(image: &[u8], model: rnse::board::Model, example: &str) -> Option<(Machine, String, Vec<u8>)> {
    use rnse::board::Model;
    let sdk = kit();
    let unit = if model == Model::Rnse192 { "192" } else { "193" };
    let variants = fw::load_variants(image).unwrap();
    let v = variants.iter().find(|v| Model::for_variant(&v.name) == Some(model))?;
    // Builds of the same example for the two tests that use it must
    // not run into each other.
    static BUILD: std::sync::Mutex<()> = std::sync::Mutex::new(());
    let built = {
        let _lock = BUILD.lock().unwrap();
        std::process::Command::new("sh").arg(sdk.join("build.sh")).args([unit, example]).output()
    };
    match built {
        Ok(out) if out.status.success() => {}
        Ok(out) => {
            // No cross compiler here: nothing to test.  A compiler that
            // is there and fails (a program that no longer fits, say)
            // is a failure, not a skip.
            let text = String::from_utf8_lossy(&out.stderr);
            assert!(text.contains("gcc: not found") || text.contains("gcc: command not found"),
                "{} for the {}: the build failed: {}", example, unit, text);
            return None;
        }
        Err(_) => return None,
    }
    let program = std::fs::read(sdk.join(format!("out/{}-{}.bin", output_name(example), unit))).unwrap();
    let (custom, places) = fw::custom_loader(image, &v.name, &program).unwrap();
    assert_eq!(places.len(), 2, "{}: the loader is on the disc in the container and as a file: {:?}", v.name, places);
    let variants = fw::load_variants(&custom).unwrap();
    let v = variants.iter().find(|x| x.name == v.name).unwrap();
    let ldm = v.module("LDM").unwrap();
    assert!(ldm.checksum_ok());

    let mut m = Machine::for_model(model);
    m.echo_console = false;
    // Whatever was in memory before: not zeroes.
    m.scramble_ram();
    m.install_boot_stub(&v.name);
    m.atapi().load(custom.clone());
    m.start_loader(ldm, [0; 3]);
    // As unforgiving as the emulator gets (`--strict`).
    m.busy_start();
    m.watchdog_always = true;
    m.cache_model();
    Some((m, v.name.clone(), custom))
}

/// An SDK example on a disc with a higher software index than the
/// installed one, and the installed (stock) software run until its
/// application has checked the disc, copied the program to RAM and is
/// about to call its first entry point.  That is how a real unit would
/// come to run it.  None when there is no cross compiler or the disc
/// has no software for the unit.
fn started_by_stock_software(image: &[u8], model: rnse::board::Model, example: &str) -> Option<(Machine, String, Vec<u8>)> {
    started_by_stock_software_with(image, model, example, None)
}

/// The same, with the disc in the drive from the start (None) or put
/// in after so many milliseconds, as someone would do it.
fn started_by_stock_software_with(image: &[u8], model: rnse::board::Model, example: &str, insert_at: Option<u64>) -> Option<(Machine, String, Vec<u8>)> {
    use rnse::board::Model;
    let sdk = kit();
    let variants = fw::load_variants(image).unwrap();
    let v = variants.iter().find(|v| Model::for_variant(&v.name) == Some(model))?;
    sdk_example(image, model, example)?;
    let unit = if model == Model::Rnse192 { "192" } else { "193" };
    let program = std::fs::read(sdk.join(format!("out/{}-{}.bin", output_name(example), unit))).unwrap();
    let (mut custom, _) = fw::custom_loader(image, &v.name, &program).unwrap();
    // The disc's index, its two middle digits one up: those are what
    // the application compares.
    let index = fw::IndexInfo::from_disc(image, &v.name).unwrap();
    let middle: u32 = index.sofidx[1..3].parse().unwrap();
    let higher = format!("{}{:02}{}", &index.sofidx[..1], middle + 1, &index.sofidx[3..]);
    assert_eq!(fw::set_disc_index(&mut custom, &v.name, &higher).unwrap(), index.sofidx);

    // The stock software's own start of a loader can fail: on the
    // European 192 the copy of the loader goes over a variable of the
    // module APFM, whose timer handler (every 100 ms) then meets an
    // address error if it runs in the 15 ms before the loader has
    // taken the unit over (docs/boot-rom.md).  Whether it does is a
    // matter of timing, which on a real unit the drive makes vary and
    // which here is the same in every run.  So the start is tried at
    // the nominal ratio of CPU speed to timers and, when exactly that
    // happens, at the two next to it.
    let apfm = v.module("APFM").map(|a| a.load_addr | 0x8000_0000..(a.load_addr | 0x8000_0000) + a.size);
    let nominal = if model == Model::Rnse192 { 4 } else { 8 };
    let mut started = None;
    for ratio in [nominal, nominal + 1, nominal - 1] {
        let mut m = Machine::for_model(model);
        m.set_cpu_ratio(ratio);
        m.echo_console = false;
        // The CPU's caches are modelled all the way: the stock software has
        // to live with them as on a real unit, and so has the program.
        m.scramble_ram();
        m.cache_model();
        m.install_boot_stub(&v.name);
        m.install(v, &index);
        if insert_at.is_none() {
            m.atapi().load(custom.clone());
        }
        m.start_os(v).unwrap();
        m.set_ignition(true);
        let ms = m.cycles_per_ms();
        let entry = fw::load_variants(&custom).unwrap().iter().find(|x| x.name == v.name).unwrap().module("LDM").unwrap().entries[0];
        m.cpu.breakpoints.push(entry);
        let mut stop = Stop::Budget;
        if let Some(at) = insert_at {
            stop = m.run(at * ms);
            assert_eq!(stop, Stop::Budget, "{}: {:?}", v.name, m.cpu.fault);
            m.atapi().insert(Some(custom.clone()));
        }
        for _ in 0..40 {
            if stop != Stop::Budget {
                break;
            }
            stop = m.run(1_000 * ms);
        }
        let collision = stop == Stop::Fault && m.cpu.fault.as_deref().is_some_and(|f| {
            let spc = f.split("SPC=").nth(1).and_then(|t| u32::from_str_radix(&t[..8.min(t.len())], 16).ok());
            f.contains("EXPEVT=0e0") && spc.zip(apfm.clone()).is_some_and(|(spc, range)| range.contains(&spc))
        });
        if collision {
            eprintln!("{}: at CPU ratio {} the stock software's start of the loader collides with its own timer handler", v.name, ratio);
            continue;
        }
        assert_eq!(stop, Stop::Breakpoint, "{}: the installed software did not start the program: {:?}", v.name, m.cpu.fault);
        started = Some(m);
        break;
    }
    let mut m = started.unwrap_or_else(|| panic!("{}: the stock software's start of the loader failed at every speed tried", v.name));
    m.cpu.breakpoints.clear();
    Some((m, v.name.clone(), custom))
}

/// Run until the log has `what` after `from`, in steps of five seconds.
fn until(m: &mut Machine, from: usize, what: &str, seconds: u64) -> Option<usize> {
    let ms = m.cycles_per_ms();
    for _ in 0..seconds / 5 {
        assert_eq!(m.run(5_000 * ms), Stop::Budget, "{:?}", m.cpu.fault);
        let text = String::from_utf8_lossy(&m.console);
        if let Some(at) = text.get(from..).and_then(|t| t.find(what)) {
            return Some(from + at);
        }
    }
    None
}

/// A boot ROM read from a 192 (`rnse report --rom`; firmware, so not in
/// the repository): `RNSE_BOOT_ROM` names the file.
fn boot_rom() -> Option<Vec<u8>> {
    let path = std::env::var_os("RNSE_BOOT_ROM")?;
    Some(std::fs::read(path).expect("RNSE_BOOT_ROM is not readable"))
}

/// A 192 as `started_192` makes it, under a unit's boot ROM, with the
/// software of `flash` installed and `drive` in the drive.
fn rom_192(flash: &[u8], drive: &[u8], rom: &[u8]) -> Option<Machine> {
    use rnse::board::Model;
    let variants = fw::load_variants(flash).unwrap();
    let v = variants.iter().find(|v| Model::for_variant(&v.name) == Some(Model::Rnse192))?;
    let index = fw::IndexInfo::from_disc(flash, &v.name).unwrap();
    let mut m = Machine::new();
    m.echo_console = false;
    m.scramble_ram();
    m.cache_model();
    m.install_boot_stub(&v.name);
    m.install(v, &index);
    m.atapi().load(drive.to_vec());
    m.set_ignition(true);
    if let Some(address) = Machine::known_printf("V2220371") {
        m.hook_printf(address);
    }
    m.start_from_reset(rom).unwrap();
    Some(m)
}

fn stock_is_in_flash(m: &Machine, image: &[u8]) {
    use rnse::board::Model;
    let variants = fw::load_variants(image).unwrap();
    let v = variants.iter().find(|v| Model::for_variant(&v.name) == Some(Model::Rnse192)).unwrap();
    for module in v.modules.iter().filter(|x| x.kind != "LDR") {
        let at = module.phys_addr() as usize;
        assert!(m.board.flash[at..at + module.size as usize] == module.data[..module.size as usize], "{} is not the stock module", module.name);
    }
}

/// Linux from an SD card (docs/linux.md): the stock software starts
/// sdboot from the disc, sdboot starts linuxboot (RNSE.BIN) from the
/// card, and linuxboot loads the kernel (VMLINUX.BIN, as build.sh
/// makes it; RNSE_LINUX names the file) and enters it.  The kernel comes
/// up on the 192's own processor with its caches modelled, starts
/// BusyBox and the GUI (rnse-gui), and keeps the board's watchdog served
/// with room to spare.  The GUI's "Shell" goes to the shell on the second
/// console, where a command typed with nothing but the knob, SELECT and
/// the soft keys ("rotary typing") runs; INFO comes back.
/// The controls work throughout (CLAUDE.md,
/// "Mandatory hardware behavior"): EJECT folds the display down and up
/// while linuxboot reads the card and under Linux, CD EJECT pushes the
/// disc out, a short RETURN does nothing of the kind, and RETURN held for
/// two seconds goes back to sdboot, which does not start Linux again by
/// itself.  Without RNSE_LINUX nothing is tested.
#[test]
fn linux_boots_from_an_sd_card() {
    use rnse::board::Model;
    let Some(image) = disc() else { return };
    let Some(kernel) = std::env::var_os("RNSE_LINUX") else { return };
    let kernel = std::fs::read(kernel).expect("RNSE_LINUX is not readable");
    let sdk = kit();
    let Some((mut m, name, _)) = started_by_stock_software(&image, Model::Rnse192, "sdboot") else { return };
    if sdk_example(&image, Model::Rnse192, &own("linuxboot")).is_none() { return }
    let linuxboot = std::fs::read(sdk.join("out/linuxboot-192.bin")).unwrap();
    let entries = vec![rnse::fat::Entry::file("RNSE.BIN", linuxboot), rnse::fat::Entry::file("VMLINUX.BIN", kernel)];
    let card = rnse::fat::card("LINUX", &entries, None).unwrap();
    assert!(m.insert_card(0, rnse::dev::sdhi::Card::from_bytes(card).unwrap()));
    let ms = m.cycles_per_ms();
    m.cpu.breakpoints.clear();
    // The watchdog from here on: the stock software's own start (some
    // 33 ms to its first edge, with the watchdog armed from power-on)
    // is not what this test judges.
    m.watchdog.longest = 0;
    let console = |m: &Machine| String::from_utf8_lossy(&m.console).into_owned();
    let press = |m: &mut Machine, key: &str, hold: u64| {
        assert!(m.panel_key(key, true));
        assert_eq!(m.run(hold * ms), Stop::Budget, "{:?}", m.cpu.fault);
        assert!(m.panel_key(key, false));
    };

    // While the card is read: the display folds down and up again.
    for _ in 0..200 {
        if console(&m).contains("READING VMLINUX.BIN") { break }
        assert_eq!(m.run(100 * ms), Stop::Budget, "{}: {:?}", name, m.cpu.fault);
    }
    assert!(console(&m).contains("READING VMLINUX.BIN"), "{}: {:?}", name, console(&m));
    press(&mut m, "EJECT", 200);
    assert_eq!(m.run(2_000 * ms), Stop::Budget, "{}: {:?}", name, m.cpu.fault);
    assert!(m.board.display_pos <= 94, "{}: the display is at {} while the card is read", name, m.board.display_pos);
    assert!(!console(&m).contains("STARTING LINUX"), "{}: the card was read too fast to try the keys", name);
    press(&mut m, "EJECT", 200);
    assert_eq!(m.run(2_000 * ms), Stop::Budget, "{}: {:?}", name, m.cpu.fault);
    assert!(m.board.display_pos >= 894, "{}: the display is at {}", name, m.board.display_pos);

    // The card's start, the kernel from it, the kernel's start, BusyBox
    // and the GUI.
    until(&mut m, 0, "rnse: Linux on the RNS-E 192 (sd start)", 90).unwrap_or_else(|| panic!("{}: Linux did not come up: {:?}", name, console(&m)));
    assert_eq!(m.run(6_000 * ms), Stop::Budget, "{}: {:?}", name, m.cpu.fault);
    let text = console(&m);
    let tail = &text[text.len().saturating_sub(600)..];
    assert!(text.contains("rnse sdk: linuxboot"), "{}: {:?}", name, tail);
    assert!(text.contains("Booting machvec: Audi RNS-E 192"), "{}: {:?}", name, tail);
    assert!(text.contains("CPU: SH7750R"), "{}: {:?}", name, tail);
    assert!(text.contains("fb0: 400x235"), "{}: {:?}", name, tail);
    assert!(text.contains("return to the loader with RETURN held"), "{}: {:?}", name, tail);
    assert!(!text.contains("Kernel panic") && !text.contains("Oops"), "{}: {:?}", name, tail);
    let (_, _, pixels) = m.render();
    assert!(pixels.iter().any(|&p| p != 0), "{}: the GUI's screen is dark", name);

    // The GUI's "Shell", the sixth of its list, and there, typed with the
    // knob, a command whose answer comes out on the serial console, the
    // kernel's log.
    let ready = until(&mut m, 0, "rnse-gui: ready", 30).unwrap_or_else(|| panic!("{}: the GUI did not come up: {:?}", name, console(&m)));
    assert!(m.panel_knob("main", 5));
    assert_eq!(m.run(300 * ms), Stop::Budget, "{}: {:?}", name, m.cpu.fault);
    press(&mut m, "SELECT", 100);
    until(&mut m, ready, "rnse-gui: shell", 10).unwrap_or_else(|| panic!("{}: the GUI did not go to the shell: {:?}", name, console(&m)));
    assert_eq!(m.run(500 * ms), Stop::Budget, "{}: {:?}", name, m.cpu.fault);
    const RING: &str = "abcdefghijklmnopqrstuvwxyz0123456789-./_|>&;=*~$'\"<()[]{}!?#@%+:,\\^`ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    let edges = m.board.watchdog_edges;
    let mut at = 0;
    for c in "echo rotary>/dev/kmsg".chars() {
        if c == ' ' {
            press(&mut m, "SOFT_BL", 100);
        } else {
            let target = RING.find(c).unwrap();
            // The first detent shows the character where the ring stands.
            let detents = (target + RING.len() - at) % RING.len() + 1;
            assert!(m.panel_knob("main", detents as i32));
            assert_eq!(m.run(100 * ms), Stop::Budget, "{}: {:?}", name, m.cpu.fault);
            press(&mut m, "SELECT", 100);
            at = target;
        }
        assert_eq!(m.run(50 * ms), Stop::Budget, "{}: {:?}", name, m.cpu.fault);
    }
    press(&mut m, "SOFT_BR", 100);
    assert_eq!(m.run(2_000 * ms), Stop::Budget, "{}: {:?}", name, m.cpu.fault);
    let text = console(&m);
    assert!(text.lines().any(|l| l.trim_end().ends_with("] rotary")), "{}: the typed command did not run: {:?}", name, &text[text.len().saturating_sub(600)..]);
    assert!(m.board.watchdog_edges > edges + 500, "{}: {} edges while typing", name, m.board.watchdog_edges - edges);
    let (_, _, pixels) = m.render();
    assert!(pixels.iter().any(|&p| p != 0), "{}: the screen is dark", name);
    let shell = console(&m).len();
    press(&mut m, "INFO", 100);
    until(&mut m, shell, "rnse-gui: back", 10).unwrap_or_else(|| panic!("{}: INFO did not bring the GUI back", name));

    // Under Linux: a short RETURN stays in Linux; the display folds down,
    // the disc comes out, the display folds up.
    press(&mut m, "RETURN", 300);
    assert_eq!(m.run(1_000 * ms), Stop::Budget, "{}: {:?}", name, m.cpu.fault);
    assert!(!console(&m).contains("back to the SD loader"), "{}: a short RETURN left Linux", name);
    press(&mut m, "EJECT", 200);
    assert_eq!(m.run(2_500 * ms), Stop::Budget, "{}: {:?}", name, m.cpu.fault);
    assert!(m.board.display_pos <= 94, "{}: the display is at {} under Linux", name, m.board.display_pos);
    let ejects = m.atapi().ejects;
    press(&mut m, "CD_EJECT", 200);
    assert_eq!(m.run(2_000 * ms), Stop::Budget, "{}: {:?}", name, m.cpu.fault);
    let text = console(&m);
    assert_eq!(m.atapi().ejects, ejects + 1, "{}: no eject: {:?}", name, &text[text.len().saturating_sub(300)..]);
    press(&mut m, "EJECT", 200);
    assert_eq!(m.run(2_500 * ms), Stop::Budget, "{}: {:?}", name, m.cpu.fault);
    assert!(m.board.display_pos >= 894, "{}: the display is at {}", name, m.board.display_pos);

    // RETURN held: back to sdboot, which stays there.
    press(&mut m, "RETURN", 2_500);
    assert_eq!(m.run(3_000 * ms), Stop::Budget, "{}: {:?}", name, m.cpu.fault);
    let text = console(&m);
    let back = text.find("back to the SD loader").unwrap_or_else(|| panic!("{}: RETURN held did not leave Linux", name));
    assert!(text[back..].contains("rnse sdk: sdboot") && text[back..].contains("TEST RETURNED: 0"), "{}: {:?}", name, &text[back..]);
    assert!(!text[back..].contains("rnse sdk: linuxboot"), "{}: Linux was started again", name);

    // The unit resets 105 ms after the last edge: keep well inside.
    // The emulator counts an instruction as a cycle, which is kind to
    // loops of cache operations: a pause of 50 ms here, on the way back
    // (the purge of all RAM from the cache, unstrobed), was a reset on a
    // unit, which then came up in the stock software.  Normal is the
    // tick's spacing, some 16 ms; under Linux the console's scrolling
    // (written with interrupts off) makes 33, which is the next thing
    // to bring down.
    let longest = m.watchdog.longest / ms;
    assert!(longest < 40, "{}: the watchdog waited {} ms, until {} ms, PC {:08x}", name, longest, m.watchdog.longest_end / ms, m.watchdog.longest_pc);
    let cache = &m.cpu.cache.trouble;
    assert_eq!((cache.stale_fetches, cache.dirty_discarded), (0, 0), "{}: {:?}", name, cache);
}

/// The SD path to Linux (as `linux_boots_from_an_sd_card`) with a card
/// that also holds files and a script, AUTORUN.SH, which /etc/init.d/rcS
/// runs: every driver of docs/linux.md from user space.  The flash and
/// the battery-backed memory read, the A/D converter, the card itself, the
/// radio processor answering, CAN frames out on the bus (and the bus's
/// frames in), the disc in the drive mounted and read, sound from a WAV
/// and an MP3 file through a card processor's converter.  Then the GUI's
/// pages, with the keys; and the disc comes out and the display folds
/// down while sound plays.
#[test]
fn linux_drivers_and_gui_from_an_sd_card() {
    use rnse::board::Model;
    let Some(image) = disc() else { return };
    let Some(kernel) = std::env::var_os("RNSE_LINUX") else { return };
    let kernel = std::fs::read(kernel).expect("RNSE_LINUX is not readable");
    let sdk = kit();
    let Some((mut m, name, _)) = started_by_stock_software(&image, Model::Rnse192, "sdboot") else { return };
    if sdk_example(&image, Model::Rnse192, &own("linuxboot")).is_none() { return }
    let linuxboot = std::fs::read(sdk.join("out/linuxboot-192.bin")).unwrap();
    let mut wav = Vec::new();
    let samples: Vec<i16> = (0..22050).map(|i| ((i as f64 * 440.0 * std::f64::consts::TAU / 22050.0).sin() * 8000.0) as i16).collect();
    wav.extend_from_slice(b"RIFF");
    wav.extend_from_slice(&(36 + 2 * samples.len() as u32).to_le_bytes());
    wav.extend_from_slice(b"WAVEfmt ");
    for v in [16u32, 0x0001_0001, 22050, 44100, 0x0010_0002] {
        wav.extend_from_slice(&v.to_le_bytes());
    }
    wav.extend_from_slice(b"data");
    wav.extend_from_slice(&(2 * samples.len() as u32).to_le_bytes());
    for v in &samples {
        wav.extend_from_slice(&v.to_le_bytes());
    }
    let script = b"echo \"== T: mode $(cat /sys/kernel/rnse/mode)\"
echo \"== T: mtd $(grep -c mtd /proc/mtd)\"
echo \"== T: sram $(dd if=/sys/bus/nvmem/devices/rnse-sram/nvmem bs=1 count=4 skip=261888 2>/dev/null | wc -c)\"
echo \"== T: adc $(cat /sys/bus/iio/devices/iio:device0/in_voltage3_raw)\"
echo \"== T: card $(cat /sys/block/rnsesd0/size)\"
echo \"== T: radio $(rnse-radio version)\"
rnse-can send 7f0#0102 && echo \"== T: can sent $(cat /sys/class/net/can0/statistics/tx_packets)\"
echo \"== T: can received $(rnse-can dump 2 | wc -l)\"
mount -t iso9660 -o ro /dev/sr0 /media/cd && echo \"== T: cd $(ls /media/cd | grep -c LOADING.KWI)\"
umount /media/cd
rnse-radio source decoder 12
rnse-play /media/sd0/TONE.WAV /media/sd0/TONES.MP3 && echo \"== T: played\"
echo \"== T: done\"
".to_vec();
    let entries = vec![
        rnse::fat::Entry::file("RNSE.BIN", linuxboot),
        rnse::fat::Entry::file("VMLINUX.BIN", kernel),
        rnse::fat::Entry::file("TONE.WAV", wav),
        rnse::fat::Entry::file("TONES.MP3", include_bytes!("data/tones.mp3").to_vec()),
        rnse::fat::Entry::file("AUTORUN.SH", script),
    ];
    let card = rnse::fat::card("LINUX", &entries, None).unwrap();
    assert!(m.insert_card(0, rnse::dev::sdhi::Card::from_bytes(card).unwrap()));
    let ms = m.cycles_per_ms();
    m.cpu.breakpoints.clear();
    m.watchdog.longest = 0;
    let console = |m: &Machine| String::from_utf8_lossy(&m.console).into_owned();
    let press = |m: &mut Machine, key: &str, hold: u64| {
        assert!(m.panel_key(key, true));
        assert_eq!(m.run(hold * ms), Stop::Budget, "{:?}", m.cpu.fault);
        assert!(m.panel_key(key, false));
    };
    let start = until(&mut m, 0, "rnse: running /media/sd0/AUTORUN.SH", 120).unwrap_or_else(|| panic!("{}: the card's script did not run: {:?}", name, console(&m)));
    let played = until(&mut m, start, "== T: played", 90);
    let text = console(&m);
    let log = &text[start..];
    assert!(played.is_some(), "{}: {:?}", name, log);
    for (what, want) in [("mode", "sd"), ("mtd", "7"), ("sram", "4"), ("adc", "1023"), ("can sent", "1"), ("can received", "2"), ("cd", "1")] {
        let line = log.lines().find(|l| l.starts_with(&format!("== T: {} ", what))).unwrap_or_else(|| panic!("{}: no {}: {:?}", name, what, log));
        assert_eq!(line[format!("== T: {} ", what).len()..].trim(), want, "{}: {}", name, line);
    }
    assert!(log.contains("== T: radio A2220141"), "{}: the radio processor did not answer: {:?}", name, log);
    assert!(log.lines().any(|l| l.starts_with("== T: card ") && l[11..].trim().parse::<u64>().unwrap_or(0) > 0), "{}: no card: {:?}", name, log);
    assert!(m.can().tx.iter().any(|f| f.id == 0x7f0 && f.data[..2] == [1, 2]), "{}: 7f0 not on the bus", name);
    let loud = m.board.media.dsps[0].loud_frames + m.board.media.dsps[1].loud_frames;
    assert!(loud > 44_100, "{}: the converters played {} frames that were not silent", name, loud);
    assert!(m.radio.source == 4, "{}: the amplifier's source is {}, not the card processors", name, m.radio.source);

    // The GUI's pages, by their keys.
    for key in ["RADIO", "MEDIA", "CAR", "SETUP", "INFO"] {
        press(&mut m, key, 100);
        assert_eq!(m.run(1_500 * ms), Stop::Budget, "{}: {:?}", name, m.cpu.fault);
        let (_, _, pixels) = m.render();
        assert!(pixels.iter().any(|&p| p != 0), "{}: the screen is dark after {}", name, key);
    }
    // Sound from the GUI (Media, the first card, its first file), and
    // while it plays the disc comes out and the display folds down.
    press(&mut m, "MEDIA", 100);
    assert_eq!(m.run(1_000 * ms), Stop::Budget);
    press(&mut m, "SELECT", 100);
    assert_eq!(m.run(1_000 * ms), Stop::Budget);
    let before = m.board.media.dsps[0].loud_frames + m.board.media.dsps[1].loud_frames;
    press(&mut m, "SELECT", 100);
    assert_eq!(m.run(2_000 * ms), Stop::Budget);
    let after = m.board.media.dsps[0].loud_frames + m.board.media.dsps[1].loud_frames;
    assert!(after > before, "{}: nothing played from the GUI", name);
    let ejects = m.atapi().ejects;
    press(&mut m, "CD_EJECT", 200);
    assert_eq!(m.run(4_000 * ms), Stop::Budget, "{}: {:?}", name, m.cpu.fault);
    assert!(m.board.display_pos <= 94, "{}: the display is at {}", name, m.board.display_pos);
    assert_eq!(m.atapi().ejects, ejects + 1, "{}: the disc did not come out while sound played", name);
    // RETURN held: playback stops and sdboot is back.
    press(&mut m, "RETURN", 2_500);
    assert_eq!(m.run(3_000 * ms), Stop::Budget, "{}: {:?}", name, m.cpu.fault);
    let text = console(&m);
    let back = text.find("back to the SD loader").unwrap_or_else(|| panic!("{}: RETURN held did not leave Linux", name));
    assert!(text[back..].contains("TEST RETURNED: 0"), "{}: {:?}", name, &text[back..]);
    assert!(!text.contains("Kernel panic") && !text.contains("Oops") && !text.contains("BUG:"), "{}", name);
    let longest = m.watchdog.longest / ms;
    assert!(longest < 40, "{}: the watchdog waited {} ms, until {} ms, PC {:08x}", name, longest, m.watchdog.longest_end / ms, m.watchdog.longest_pc);
    let cache = &m.cpu.cache.trouble;
    assert_eq!((cache.stale_fetches, cache.dirty_discarded), (0, 0), "{}: {:?}", name, cache);
}

/// The disc `rnse firmware ... --kernel` makes: linuxboot in the
/// operating system's place, the kernel (RNSE_LINUX) in the application's,
/// the swap loader, and a higher index than the stock disc's.  With
/// `spoil`, one byte of the kernel changed after its sum was taken.
fn linux_firmware_disc(image: &[u8], spoil: bool) -> Option<Vec<u8>> {
    use rnse::board::Model;
    let kernel = std::fs::read(std::env::var_os("RNSE_LINUX")?).expect("RNSE_LINUX is not readable");
    let sdk = kit();
    let variants = fw::load_variants(image).unwrap();
    let v = variants.iter().find(|v| Model::for_variant(&v.name) == Some(Model::Rnse192))?;
    sdk_example(image, Model::Rnse192, &own("linuxboot"))?;
    let program = std::fs::read(sdk.join("out/linuxboot-192.bin")).unwrap();
    sdk_example(image, Model::Rnse192, "swap")?;
    let swap = std::fs::read(sdk.join("out/swap-192.bin")).unwrap();
    let mut disc = fw::firmware_disc_with_kernel(image, &v.name, &program, Some(&swap), Some(&kernel)).unwrap();
    if spoil {
        let apm = v.module("APM").unwrap();
        let mut spoilt = fw::kernel_module(&apm.data, &kernel).unwrap();
        spoilt[fw::KERNEL_AT + 0x3000] ^= 0x40;
        let name = v.name.clone();
        disc = fw::repack(&disc, &|var, module| (var == name && module == "APM").then(|| spoilt.clone())).unwrap().0;
    }
    let index = fw::IndexInfo::from_disc(image, &v.name).unwrap();
    let middle: u32 = index.sofidx[1..3].parse().unwrap();
    let higher = format!("{}{:02}{}", &index.sofidx[..1], middle + 1, &index.sofidx[3..]);
    fw::set_disc_index(&mut disc, &v.name, &higher).unwrap();
    Some(disc)
}

/// Linux installed in flash (docs/linux.md, "Linux in flash"), all the
/// way under a unit's boot ROM (RNSE_BOOT_ROM) with the caches modelled
/// and RAM arbitrary: the stock software takes the firmware disc for an
/// update and its loader writes linuxboot and the kernel; the ROM starts
/// linuxboot, which counts the start for the guard, checks the kernel in
/// the flash and starts it; the GUI comes up and, ten seconds in, counts
/// the start as one that worked.  The controls work, RETURN held says
/// there is no loader to go back to, the watchdog is served, and after a
/// power cycle the unit comes up in Linux again.
#[test]
fn linux_is_installed_in_flash() {
    let (Some(image), Some(rom)) = (disc(), boot_rom()) else { return };
    let Some(custom) = linux_firmware_disc(&image, false) else { return };
    let Some(mut m) = rom_192(&image, &custom, &rom) else { return };
    let ms = m.cycles_per_ms();
    let tail = |m: &Machine| { let t = String::from_utf8_lossy(&m.console).into_owned(); t[t.len().saturating_sub(600)..].to_string() };
    let press = |m: &mut Machine, key: &str, hold: u64| {
        assert!(m.panel_key(key, true));
        assert_eq!(m.run(hold * ms), Stop::Budget, "{:?}", m.cpu.fault);
        assert!(m.panel_key(key, false));
    };
    let jump = until(&mut m, 0, "*** Jump User RESET ***", 600).unwrap_or_else(|| panic!("the stock software did not install the disc: {:?}", tail(&m)));
    let started = until(&mut m, jump, "rnse sdk: linuxboot from flash", 60).unwrap_or_else(|| panic!("the ROM did not start linuxboot: {:?}", tail(&m)));
    m.watchdog.longest = 0;
    // The cache's record from here on: what the stock loader left dirty
    // when it jumped to the reset is lost on a unit too.
    let before = (m.cpu.cache.trouble.stale_fetches, m.cpu.cache.trouble.dirty_discarded);
    eprintln!("cache trouble when linuxboot starts: {:?}", m.cpu.cache.trouble);
    let up = until(&mut m, started, "rnse: Linux on the RNS-E 192 (flash start)", 60).unwrap_or_else(|| panic!("Linux did not come up from flash: {:?}", tail(&m)));
    until(&mut m, up, "rnse: this start counted as one that worked", 30).unwrap_or_else(|| panic!("the GUI did not call the start healthy: {:?}", tail(&m)));
    let text = String::from_utf8_lossy(&m.console).into_owned();
    assert!(text[started..].contains("rnse.flash=1"), "{:?}", tail(&m));
    assert!(text[started..].contains("return to the loader not available (started from flash)"), "{:?}", tail(&m));
    let (_, _, pixels) = m.render();
    assert!(pixels.iter().any(|&p| p != 0), "the screen is dark");
    // The controls: the disc out (the firmware disc is still in), the
    // display down and up; RETURN held leaves nothing.
    let ejects = m.atapi().ejects;
    press(&mut m, "CD_EJECT", 200);
    assert_eq!(m.run(4_000 * ms), Stop::Budget, "{:?}", m.cpu.fault);
    let now = m.atapi().ejects;
    assert_eq!(now, ejects + 1, "no eject: {:?}", tail(&m));
    press(&mut m, "EJECT", 200);
    assert_eq!(m.run(2_500 * ms), Stop::Budget, "{:?}", m.cpu.fault);
    assert!(m.board.display_pos >= 894, "the display is at {}", m.board.display_pos);
    press(&mut m, "RETURN", 2_500);
    assert_eq!(m.run(2_000 * ms), Stop::Budget, "{:?}", m.cpu.fault);
    assert!(String::from_utf8_lossy(&m.console).contains("return to the SD loader is not available here (started from flash)"));
    assert_eq!(m.resets, 0, "the watchdog reset the unit");
    let longest = m.watchdog.longest / ms;
    assert!(longest < 40, "the watchdog waited {} ms, until {} ms, PC {:08x}", longest, m.watchdog.longest_end / ms, m.watchdog.longest_pc);
    let erased = m.board.flash_ctl.erased_sectors;
    let cache = &m.cpu.cache.trouble;
    assert_eq!((cache.stale_fetches, cache.dirty_discarded), before, "{:?}", cache);
    // A power cycle: Linux again, nothing erased.
    m.cpu.reset();
    let again = until(&mut m, up, "rnse sdk: linuxboot from flash", 30).expect("not started again");
    until(&mut m, again, "rnse: Linux on the RNS-E 192 (flash start)", 60).expect("Linux did not come up again");
    assert_eq!(m.board.flash_ctl.erased_sectors, erased, "something was erased");
}

/// The guard (sdk/src/flash.c) is the way out of Linux in flash: with a
/// kernel that is not whole, linuxboot says so and keeps the controls
/// going but never counts a start as one that worked, and at the fifth
/// start in a row the guard takes the program out; the boot ROM installs
/// the stock disc that is in the drive.  Nothing else erases anything.
/// Each start's CD EJECT is tried while linuxboot waits; the stock disc
/// goes in before the power cycle that follows.
#[test]
fn linux_in_flash_with_a_spoilt_kernel_is_taken_out_by_the_guard() {
    let (Some(image), Some(rom)) = (disc(), boot_rom()) else { return };
    let Some(spoilt) = linux_firmware_disc(&image, true) else { return };
    let Some(mut m) = rom_192(&spoilt, &image, &rom) else { return };
    let ms = m.cycles_per_ms();
    let count = |m: &Machine| String::from_utf8_lossy(&m.console).matches("rnse sdk: linuxboot from flash").count();
    for n in 1..=4 {
        assert_eq!(m.run(4_000 * ms), Stop::Budget, "{:?}", m.cpu.fault);
        assert_eq!(count(&m), n, "start {}", n);
        let text = String::from_utf8_lossy(&m.console).into_owned();
        assert!(text.matches("NO KERNEL IN THE FLASH").count() == n, "start {}: {:?}", n, &text[text.len().saturating_sub(400)..]);
        assert!(!text.contains("Linux version"), "a spoilt kernel was started");
        assert_eq!(m.resets, 0, "the watchdog reset the unit while linuxboot waited");
        // The controls while it waits.
        let ejects = m.atapi().ejects;
        assert!(m.panel_key("CD_EJECT", true));
        assert_eq!(m.run(200 * ms), Stop::Budget);
        assert!(m.panel_key("CD_EJECT", false));
        assert_eq!(m.run(3_000 * ms), Stop::Budget);
        assert_eq!(m.atapi().ejects, ejects + 1, "start {}: CD EJECT did nothing", n);
        // Off and on with the stock disc put in: the drive comes up with
        // it, as at power-on.  (m.cpu.reset() resets the processor only;
        // a disc inserted while the unit ran would leave the emulated
        // drive with its "medium changed" still to tell, which a power
        // cut clears on a unit, and the boot ROM does not get past it.)
        m.atapi().load(image.clone());
        m.cpu.reset();
    }
    let tail = |m: &Machine| { let t = String::from_utf8_lossy(&m.console).into_owned(); t[t.len().saturating_sub(1500)..].to_string() };
    let back = until(&mut m, 0, "*** Jump User RESET ***", 600).unwrap_or_else(|| panic!("the ROM did not install the stock disc: {:?}", tail(&m)));
    assert_eq!(count(&m), 4, "linuxboot was started a fifth time");
    until(&mut m, back, "[fw] SOUND_TSK START", 60).expect("the stock software did not start");
    stock_is_in_flash(&m, &image);
}
