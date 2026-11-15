/* /init of the RNS-E 192's initramfs: built with the kernel's own nolibc
 * (tools/include/nolibc), big-endian, static, no C library and no libgcc.
 * It never divides, so that nothing pulls in libgcc's division helpers
 * (the Ubuntu toolchain's libgcc is little-endian).
 *
 * It mounts the pseudo filesystems, shows what the kernel found, and then
 * says how long the system has been up every few seconds, on every
 * console it can open: /dev/console and, once there is one, the
 * framebuffer's /dev/tty1.  That last line changing on the screen is the
 * sign of life on a unit with no serial port to look at. */
#include "nolibc.h"

static int out[2] = { -1, -1 };

static void put(const char *s, long n)
{
    int i;
    for (i = 0; i < 2; i++)
        if (out[i] >= 0)
            write(out[i], s, n);
}

static void say(const char *s)
{
    put(s, strlen(s));
}

static void show(const char *path, const char *title)
{
    char buf[512];
    long n;
    int fd = open(path, O_RDONLY);
    say(title);
    if (fd < 0) {
        say(" (cannot open)\n");
        return;
    }
    while ((n = read(fd, buf, sizeof buf)) > 0)
        put(buf, n);
    close(fd);
}

int main(void)
{
    char buf[64];
    long n;
    int fd;

    out[0] = open("/dev/console", O_WRONLY);
    mount("proc", "/proc", "proc", 0, 0);
    mount("sysfs", "/sys", "sysfs", 0, 0);
    mount("devtmpfs", "/dev", "devtmpfs", 0, 0);
    if (out[0] < 0)
        out[0] = open("/dev/console", O_WRONLY);

    say("\nrnse: Linux on the RNS-E 192, /init running\n");
    show("/proc/version", "");
    show("/proc/cpuinfo", "--- /proc/cpuinfo\n");
    show("/proc/meminfo", "--- /proc/meminfo\n");
    show("/proc/interrupts", "--- /proc/interrupts\n");
    show("/proc/cmdline", "--- /proc/cmdline\n");

    for (;;) {
        /* The framebuffer console, when there is one, besides the
         * kernel's console (which may be the serial port only). */
        if (out[1] < 0)
            out[1] = open("/dev/tty1", O_WRONLY);
        fd = open("/proc/uptime", O_RDONLY);
        if (fd >= 0) {
            n = read(fd, buf, sizeof buf);
            close(fd);
            if (n > 0) {
                say("rnse: up ");
                put(buf, n);
            }
        }
        sleep(5);
    }
    return 0;
}
