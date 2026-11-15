/* Linux from an SD card (192): the second stage after sdk/examples/sdboot.
 *
 * Copied to the card as RNSE.BIN, beside VMLINUX.BIN as linux/build.sh
 * makes it.  sdboot loads and starts this program; it reads VMLINUX.BIN
 * from the same card to 0x8c001000, where the kernel's image begins with
 * its zero page, and enters the kernel at 0x8c002000 (_stext) with
 * interrupts masked, timers and DMA stopped, the companion chip's
 * interrupt sources masked, the operand cache written back over the image
 * and the instruction cache invalidated.
 *
 * The way back: the kernel keeps its hands off sdboot's saved copy and
 * session (0x8c7c0000-0x8c800000) and off this program (0x8df00000, 128
 * KiB), and this program tells it, on its command line in the zero page,
 * where its loader_return and its stack are.  RETURN held for two seconds
 * under Linux, or here while the card is read, goes back to sdboot.
 *
 * The controls work all the while (CLAUDE.md, "Mandatory hardware
 * behavior"): the faceplate's EJECT key folds the display down and up and
 * the CD eject key behind it pushes the disc out, here through the SDK's
 * panel_service (from sd_idle while the card is read) and under Linux
 * through the kernel's own driver (arch/sh/boards/rnse-controls.c).
 *
 * Should the kernel not be there, it says so and returns to sdboot.  See
 * docs/linux.md. */
#include "rnse.h"

#define KERNEL_LOAD 0x8c001000u
#define KERNEL_ENTRY 0x8c002000u
#define KERNEL_COMMAND_LINE (KERNEL_LOAD + 0x100)
/* The kernel, its bss included, must end below sdboot's session. */
#define KERNEL_ROOM (0x8c7c0000u - KERNEL_LOAD)
/* What sdboot leaves for the program it starts (sdk/src/wayback.c). */
#define SESSION ((volatile u32 *)0xac7c0000)
#define SESSION_MAGIC 0x53444c31u

#define RETURN_HOLD_MS 2000

void irq_disable(void);
void linux_enter(u32 entry);
extern const char _stack_top[];

static int s, row;

static void say(const char *text, u8 colour)
{
    screen_fill(0, (30 + 14 * row) * s, screen_width, 12 * s, 0);
    screen_text(8 * s, (30 + 14 * row) * s, text, colour, 2 * s);
    console_puts(text);
    console_puts("\n");
    row++;
}

static void number(const char *what, u32 v)
{
    char text[40], *p = text, t[12];
    int n = 0;
    while (*what)
        *p++ = *what++;
    *p++ = ' ';
    do {
        t[n++] = (char)('0' + umod(v, 10));
        v = udiv(v, 10);
    } while (v);
    while (n)
        *p++ = t[--n];
    *p = 0;
    say(text, 1);
}

static char *hex(char *p, u32 v)
{
    int i;
    *p++ = '0';
    *p++ = 'x';
    for (i = 28; i >= 0; i -= 4)
        *p++ = "0123456789abcdef"[(v >> i) & 15];
    return p;
}

static int in_session(void)
{
    return SESSION[0] == SESSION_MAGIC && SESSION[1] == ~SESSION_MAGIC &&
           SESSION[2] == 0x8df00000u && SESSION[3] < 2 && SESSION[4] == 1;
}

/* ---- The faceplate, polled from sd_idle and while waiting ---------- */

static int return_down = 1;
static u32 return_since;

static void controls(void)
{
    panel_service(); /* the faceplate, the display key, CD EJECT */
    /* RETURN held: back to sdboot.  A RETURN still down from sdboot
     * (which reruns a program on it) has to be let go first. */
    if (!panel_key(KEY_RETURN)) {
        return_down = 0;
    } else if (!return_down) {
        return_down = 1;
        return_since = millis();
    } else if (return_since && millis() - return_since >= RETURN_HOLD_MS) {
        return_since = 0;
        loader_return(0);
        row = 7;
        say("RETURN TO THE LOADER UNAVAILABLE", 2);
    }
}

static void wait_ms(u32 ms)
{
    u32 t = millis();
    while (millis() - t < ms) {
        controls();
        cpu_idle();
    }
}

/* ---- Loading and starting the kernel ------------------------------- */

/* The slot sdboot read this program from, or -1. */
static int started_from(void)
{
    if (SESSION[0] == SESSION_MAGIC && SESSION[1] == ~SESSION_MAGIC && SESSION[3] < 2)
        return (int)SESSION[3];
    return -1;
}

static int load(int slot, u32 *size)
{
    int r = sd_attach(slot);
    if (r)
        r = sd_init(slot);
    if (r)
        return r;
    return sd_load("VMLINUX BIN", (void *)KERNEL_LOAD, KERNEL_ROOM, size);
}

int main(void)
{
    u32 size = 0, a, t0, end;
    int slot = started_from(), r = -2, i;

    board_init();
    console_puts("\nrnse sdk: linuxboot\n");
    s = screen_width / 400;
    screen_palette(1, 255, 255, 255);
    screen_palette(2, 255, 96, 0);
    screen_text(8 * s, 6 * s, "LINUX FROM AN SD CARD", 2, 2 * s);
    /* The faceplate controller as sdboot left it, lit, and answered from
     * here on: restarted, it would leave the screen dark until it had its
     * settings again (docs/custom-firmware.md, "Programs from an SD card"). */
    panel_attach();
    sd_idle = controls;
    display_idle = controls;

    t0 = millis();
    say("READING VMLINUX.BIN", 1);
    say("HOLD RETURN: BACK TO THE LOADER", 2);
    if (slot >= 0)
        r = load(slot, &size);
    else
        for (slot = 0; slot < 2 && r; slot++)
            if (sd_present(slot))
                r = load(slot, &size);
    /* The kernel's own size, bss and all, is in its image: _end - _text
     * is not, so take the file and leave the bss to the check in
     * linux/build.sh. */
    end = KERNEL_LOAD + size;
    if (r || size < 0x2000 || REG32(KERNEL_ENTRY) == 0) {
        row = 2;
        number("NO KERNEL, ERROR", (u32)-r);
        say("VMLINUX.BIN MISSING OR TOO LONG", 2);
        wait_ms(3000);
        loader_return(r ? r : -30);
        say("RETURN TO THE LOADER UNAVAILABLE", 2);
        for (;;)
            wait_ms(1000);
    }
    row = 2;
    number("BYTES", size);
    number("MS", millis() - t0);
    say("STARTING LINUX", 2);
    wait_ms(300);
    /* Linux takes the motor over from a standstill. */
    if (display_state() == DISPLAY_OPENING || display_state() == DISPLAY_CLOSING)
        display_wait(9000);
    display_halt();

    /* The way back, for the kernel: its command line in the zero page
     * (the kernel appends its own, CONFIG_CMDLINE_EXTEND). */
    {
        char *p = (char *)KERNEL_COMMAND_LINE;
        if (in_session()) {
            const char *k = "rnse.return=";
            while (*k)
                *p++ = *k++;
            p = hex(p, (u32)loader_return);
            *p++ = ',';
            p = hex(p, (u32)_stack_top);
        }
        *p = 0;
    }

    irq_disable();
    /* Timers, every DMA channel, the on-chip and companion interrupt
     * sources: as loader_return stops them, so that nothing goes on
     * writing memory or asking for interrupts under the kernel. */
    REG8(0xffd80004) = 0;
    REG16(0xffd80010) = 0;
    for (i = 0; i < 8; i++)
        REG32(0xffa0000c + (i < 4 ? i : i + 1) * 0x10) = 0;
    REG32(0xffa00040) = 0;
    REG16(0xffd00004) = 0;
    REG16(0xffd00008) = 0;
    REG16(0xffd0000c) = 0;
    for (i = 0; i <= 0x14; i += 2)
        REG16(0xa7b00000 + i) = 0;
    REG16(0xa7b0001e) = 0;
    /* The kernel's bytes from the operand cache to memory, the image and
     * the room for its bss (the kernel clears that through the cache). */
    for (a = KERNEL_LOAD; a < end + 0x80000 && a < KERNEL_LOAD + KERNEL_ROOM; a += 32)
        __asm__ volatile ("ocbp @%0" : : "r"(a) : "memory");
    REG8(0xb2300001) ^= 1; /* the watchdog, once more */
    /* Run the last step uncached: it changes the cache's control. */
    ((void (*)(u32))(((u32)linux_enter & 0x1fffffff) | 0xa0000000))(KERNEL_ENTRY);
    return -31;
}

/* Invalidate the instruction cache (CCR.ICI; from an uncached address, and
 * eight instructions before a branch to cached code, as the SH-4 manual
 * wants) and jump into the kernel.  r4 = entry. */
__asm__(
    "    .text\n"
    "    .align 2\n"
    "    .global linux_enter\n"
    "linux_enter:\n"
    "    mov.l 1f, r1\n"
    "    mov.l @r1, r0\n"
    "    mov.l 2f, r2\n"
    "    or r2, r0\n"
    "    mov.l r0, @r1\n"
    "    nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n"
    "    jmp @r4\n"
    "    nop\n"
    "    .align 2\n"
    "1:  .long 0xff00001c\n"
    "2:  .long 0x00000800\n");
