/* rnse-gui: a plain user interface for Linux on the Audi RNS-E 192, on
 * the 400x235 screen and run with the faceplate alone.
 *
 *   select knob       up and down a list         SELECT     open, do
 *   RETURN (short)    back                       volume     the amplifier's volume
 *   RADIO, MEDIA, CAR, SETUP, INFO               the radio, the media, CAN,
 *                                                the system, the start page
 *   NEXT, PREV        the next or previous track, or a step of the tuner
 *
 * EJECT (the display) and CD EJECT are the kernel's and work whatever
 * this program does; so is RETURN held for two seconds (the way back to
 * the SD loader), which this program never sees (rnse-controls.c).
 *
 * The screen: /dev/fb0, eight bits through a palette.  The Coral chip
 * is little-endian on a big-endian bus, so pixel N is the byte at N ^ 3
 * of video memory; the picture is drawn in RAM and copied a word at a
 * time with its bytes turned round.  The letters are the console's own
 * font, asked of the kernel (KDFONTOP).
 *
 * Started from flash, the program tells the kernel after its first
 * picture and ten seconds that this start worked (/sys/kernel/rnse/
 * healthy): the guard of sdk/src/flash.c counts a start that never got
 * so far, and the fifth such in a row takes Linux out of the flash. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <linux/can.h>
#include "rnse.h"

#define W 400
#define H 235

/* ---- The screen ----------------------------------------------------------- */

enum { BLACK, WHITE, ORANGE, GREY, DARK, RED, GREEN, SELECTED };

static uint8_t canvas[H][W];
static uint8_t *vram;
static int stride, fb_ok;
static uint8_t font[256][8];
static int font_ok;

static void fb_setup(void)
{
	static const uint8_t rgb[][3] = {
		{ 0, 0, 0 }, { 255, 255, 255 }, { 255, 96, 0 }, { 140, 140, 140 },
		{ 36, 36, 36 }, { 220, 30, 30 }, { 40, 200, 60 }, { 110, 36, 0 },
	};
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	uint16_t r[8], g[8], b[8];
	struct fb_cmap cmap = { 0, 8, r, g, b, NULL };
	int fd = open("/dev/fb0", O_RDWR), i;

	if (fd < 0)
		return;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) || ioctl(fd, FBIOGET_FSCREENINFO, &fix))
		return;
	stride = fix.line_length;
	vram = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (vram == MAP_FAILED)
		return;
	for (i = 0; i < 8; i++) {
		r[i] = rgb[i][0] * 257;
		g[i] = rgb[i][1] * 257;
		b[i] = rgb[i][2] * 257;
	}
	ioctl(fd, FBIOPUTCMAP, &cmap);
	fb_ok = var.xres >= W && var.yres >= H && var.bits_per_pixel == 8;
}

static void present(void)
{
	int x, y;

	if (!fb_ok)
		return;
	for (y = 0; y < H; y++) {
		uint32_t *to = (uint32_t *)(vram + y * stride);
		const uint8_t *p = canvas[y];

		for (x = 0; x < W; x += 4)
			*to++ = (uint32_t)p[x + 3] << 24 | p[x + 2] << 16 | p[x + 1] << 8 | p[x];
	}
}

static void font_setup(void)
{
	static uint8_t data[512 * 32 * 4];
	struct console_font_op op = {
		.op = KD_FONT_OP_GET, .width = 8, .height = 32, .charcount = 512, .data = data,
	};
	int c, row;

	if (ioctl(0, KDFONTOP, &op) && ioctl(open("/dev/tty0", O_RDWR), KDFONTOP, &op))
		return;
	if (op.width != 8 || op.height < 8)
		return;
	for (c = 0; c < 256; c++)
		for (row = 0; row < 8; row++)
			font[c][row] = data[c * 32 + row * (op.height > 8 ? op.height / 8 : 1)];
	font_ok = 1;
}

static void fill(int x, int y, int w, int h, int c)
{
	int i, j;

	for (j = y; j < y + h && j < H; j++)
		for (i = x; i < x + w && i < W; i++)
			if (i >= 0 && j >= 0)
				canvas[j][i] = c;
}

static int text(int x, int y, int c, int scale, const char *s)
{
	int i, j, sx, sy;

	for (; *s; s++, x += 8 * scale) {
		const uint8_t *g = font[(uint8_t)*s];

		if (!font_ok)
			continue;
		for (j = 0; j < 8; j++)
			for (i = 0; i < 8; i++)
				if (g[j] & (0x80 >> i))
					for (sy = 0; sy < scale; sy++)
						for (sx = 0; sx < scale; sx++)
							if (x + i * scale + sx < W && y + j * scale + sy < H)
								canvas[y + j * scale + sy][x + i * scale + sx] = c;
	}
	return x;
}

static void textf(int x, int y, int c, int scale, const char *fmt, ...)
{
	char buf[128];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	text(x, y, c, scale, buf);
}

static void centred(int y, int c, int scale, const char *s)
{
	text((W - (int)strlen(s) * 8 * scale) / 2, y, c, scale, s);
}

/* The frame: a title, and the soft keys' labels by their corners. */
static void frame(const char *title, const char *tl, const char *tr, const char *bl, const char *br)
{
	memset(canvas, BLACK, sizeof canvas);
	fill(0, 0, W, 22, DARK);
	centred(3, WHITE, 2, title);
	if (tl)
		text(4, 7, ORANGE, 1, tl);
	if (tr)
		text(W - 4 - 8 * (int)strlen(tr), 7, ORANGE, 1, tr);
	fill(0, H - 14, W, 14, DARK);
	if (bl)
		text(4, H - 11, ORANGE, 1, bl);
	if (br)
		text(W - 4 - 8 * (int)strlen(br), H - 11, ORANGE, 1, br);
}

/* A list of lines from y 28 on, 18 pixels each, the chosen one lit. */
#define ROWS 10
static void list(char **items, int n, int chosen, int *top)
{
	int i;

	if (chosen < *top)
		*top = chosen;
	if (chosen >= *top + ROWS)
		*top = chosen - ROWS + 1;
	for (i = 0; i < ROWS && *top + i < n; i++) {
		int y = 28 + 18 * i, k = *top + i;

		if (k == chosen)
			fill(0, y - 2, W, 18, SELECTED);
		text(12, y + 2, k == chosen ? WHITE : GREY, 1, items[k]);
	}
}

/* ---- Input -------------------------------------------------------------- */

/* The faceplate device's keys: BTN_TRIGGER_HAPPY1 + their order in
 * rnse-controls.c keymap[]. */
enum {
	K_INFO, K_NAV, K_TEL, K_NAME, K_MEDIA, K_RADIO, K_SOFT_TL, K_SOFT_TR,
	K_SOFT_BL, K_SOFT_BR, K_SETUP, K_CAR, K_NEXT, K_PREV, K_SELECT,
	K_RETURN, K_DISPLAY, K_CD_EJECT,
};

static int faceplate = -1, keyboard = -1;

static int find_input(const char *name)
{
	char path[32], got[64];
	int i, fd;

	for (i = 0; i < 16; i++) {
		snprintf(path, sizeof path, "/dev/input/event%d", i);
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof got), got) > 0 && !strcmp(got, name))
			return fd;
		close(fd);
	}
	return -1;
}

/* ---- What is shared ------------------------------------------------------- */

static int volume = 7;
static int frequency = 2042;	/* 102.1 MHz */
static long volume_shown;
static int radio_ok;

/* The settings in Linux's part of the battery-backed memory (0x3f640,
 * board-rnse.c): "RLX1", frequency, volume, a check byte. */
#define NVMEM "/sys/bus/nvmem/devices/rnse-sram/nvmem"
#define SETTINGS 0x3f640

static void settings_load(void)
{
	uint8_t b[8];
	int fd = open(NVMEM, O_RDONLY);

	if (fd < 0)
		return;
	if (pread(fd, b, 8, SETTINGS) == 8 && !memcmp(b, "RLX1", 4) &&
	    (uint8_t)(b[4] + b[5] + b[6] + 0x5a) == b[7]) {
		frequency = b[4] << 8 | b[5];
		volume = b[6] <= 30 ? b[6] : 7;
		if (frequency < 1750 || frequency > 2160)
			frequency = 2042;
	}
	close(fd);
}

static void settings_save(void)
{
	uint8_t b[8] = { 'R', 'L', 'X', '1', frequency >> 8, frequency, volume, 0 };
	int fd = open(NVMEM, O_WRONLY);

	b[7] = b[4] + b[5] + b[6] + 0x5a;
	if (fd < 0)
		return;
	if (pwrite(fd, b, 8, SETTINGS) != 8)
		;
	close(fd);
}

/* Playing: a child, rnse-play. */
static pid_t player;
static char playing[96];
static char queue_dir[256];
static char **queue;
static int queue_n, queue_at;

static void stop_playing(void)
{
	if (player > 0) {
		kill(player, SIGTERM);
		waitpid(player, NULL, 0);
	}
	player = 0;
	playing[0] = 0;
}

static void play_at(int k)
{
	char path[512];

	stop_playing();
	if (k < 0 || k >= queue_n)
		return;
	queue_at = k;
	snprintf(path, sizeof path, "%s/%s", queue_dir, queue[k]);
	if (radio_ok)
		radio_source(RADIO_DECODER, volume);
	player = fork();
	if (player == 0) {
		int null = open("/dev/null", O_WRONLY);

		dup2(null, 1);
		dup2(null, 2);
		execl("/usr/bin/rnse-play", "rnse-play", path, (char *)NULL);
		_exit(127);
	}
	snprintf(playing, sizeof playing, "%s", queue[k]);
}

/* A finished track: the next one. */
static void reap(void)
{
	int st;

	if (player > 0 && waitpid(player, &st, WNOHANG) == player) {
		player = 0;
		if (queue_at + 1 < queue_n)
			play_at(queue_at + 1);
		else
			playing[0] = 0;
	}
}

/* Cards and the disc mounted where the media page looks. */
static int mounted(const char *where)
{
	char buf[4096], *p;
	int n = read_file("/proc/mounts", buf, sizeof buf);

	if (n <= 0)
		return 0;
	for (p = buf; (p = strstr(p, where)); p++)
		if (p[-1] == ' ' && p[strlen(where)] == ' ')
			return 1;
	return 0;
}

static void automount(void)
{
	static const char *const at[] = { "/media/sd0", "/media/sd1" };
	char dev[32], sys[48];
	struct stat st;
	int i;

	for (i = 0; i < 2; i++) {
		snprintf(sys, sizeof sys, "/sys/block/rnsesd%d", i);
		if (stat(sys, &st)) {
			if (mounted(at[i]))
				umount2(at[i], MNT_DETACH);	/* the card has gone */
			continue;
		}
		if (mounted(at[i]))
			continue;
		mkdir("/media", 0755);
		mkdir(at[i], 0755);
		snprintf(dev, sizeof dev, "/dev/rnsesd%dp1", i);
		if (stat(dev, &st))
			snprintf(dev, sizeof dev, "/dev/rnsesd%d", i);
		mount(dev, at[i], "vfat", MS_RDONLY, "shortname=mixed");
	}
}

static int disc_in(void)
{
	char buf[32];

	return read_file("/sys/block/sr0/size", buf, sizeof buf) > 0 && atol(buf) > 0;
}

static int disc_mount(void)
{
	mkdir("/media", 0755);
	mkdir("/media/cd", 0755);
	if (mounted("/media/cd"))
		return 0;
	if (!mount("/dev/sr0", "/media/cd", "iso9660", MS_RDONLY, ""))
		return 0;
	return mount("/dev/sr0", "/media/cd", "udf", MS_RDONLY, "");
}

/* ---- Pages ---------------------------------------------------------------- */

enum { HOME, RADIO, MEDIA, CD, CAN, SYSTEM };
static int page = HOME, chosen, top;
static char status_line[64];
static long status_until;

static void say(const char *s)
{
	snprintf(status_line, sizeof status_line, "%s", s);
	status_until = now_ms() + 3000;
}

static void footer_status(void)
{
	if (now_ms() < status_until)
		centred(H - 30, ORANGE, 1, status_line);
	if (now_ms() - volume_shown < 1500) {
		char v[24];

		snprintf(v, sizeof v, "VOLUME %d", volume);
		fill(W / 2 - 70, 90, 140, 40, DARK);
		centred(102, WHITE, 2, v);
	}
}

static char *home_items[] = { "Radio", "Media", "CD", "CAN", "System", "Shell" };

static void draw_home(void)
{
	frame("RNS-E LINUX", NULL, NULL, NULL, NULL);
	list(home_items, 6, chosen, &top);
	if (playing[0]) {
		char s[64];

		snprintf(s, sizeof s, "PLAYING %s", playing);
		text(12, H - 32, GREEN, 1, s);
	}
}

/* Radio */
static long tune_at;
static uint8_t radio_text[9];

static void draw_radio(void)
{
	char f[16];

	frame("RADIO", "FM", radio_ok ? "" : "NO RADIO", NULL, NULL);
	snprintf(f, sizeof f, "%d.%d", frequency / 20, frequency % 20 * 5 / 10);
	centred(70, WHITE, 5, f);
	centred(118, GREY, 2, "MHz");
	centred(150, ORANGE, 2, (char *)radio_text);
	text(12, H - 32, GREY, 1, "KNOB 0.1 MHz  NEXT/PREV 1 MHz");
}

static void radio_poll(void)
{
	uint8_t st[64];
	static long last;

	if (!radio_ok)
		return;
	if (tune_at && now_ms() >= tune_at) {
		tune_at = 0;
		radio_tune(frequency);
		settings_save();
	}
	if (now_ms() - last > 3000) {
		last = now_ms();
		if (radio_status(st, sizeof st) >= 16) {
			memcpy(radio_text, st + 8, 8);
			radio_text[8] = 0;
		}
	}
}

/* Media: a list of a directory's sub-directories and files to play. */
static char media_dir[256] = "/media";
static char **entries;
static int entries_n;

static int playable(const char *n)
{
	const char *dot = strrchr(n, '.');

	return dot && (!strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".wav"));
}

static int by_name(const void *a, const void *b)
{
	return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static void media_read(void)
{
	DIR *d;
	struct dirent *e;
	int i;

	for (i = 0; i < entries_n; i++)
		free(entries[i]);
	free(entries);
	entries = NULL;
	entries_n = 0;
	if (!strcmp(media_dir, "/media")) {
		/* The top: what is there to play from, cards and the disc. */
		static const char *const at[] = { "sd0", "sd1", "cd" };
		char path[32];

		if (disc_in())
			disc_mount();
		for (i = 0; i < 3; i++) {
			snprintf(path, sizeof path, "/media/%s", at[i]);
			if (!mounted(path))
				continue;
			entries = realloc(entries, sizeof *entries * (entries_n + 1));
			snprintf(path, sizeof path, "%s/", at[i]);
			entries[entries_n++] = strdup(path);
		}
		return;
	}
	d = opendir(media_dir);
	if (!d)
		return;
	while ((e = readdir(d))) {
		char path[512], name[260];
		struct stat st;

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof path, "%s/%s", media_dir, e->d_name);
		if (stat(path, &st))
			continue;
		if (S_ISDIR(st.st_mode))
			snprintf(name, sizeof name, "%s/", e->d_name);
		else if (playable(e->d_name))
			snprintf(name, sizeof name, "%s", e->d_name);
		else
			continue;
		entries = realloc(entries, sizeof *entries * (entries_n + 1));
		entries[entries_n++] = strdup(name);
	}
	closedir(d);
	qsort(entries, entries_n, sizeof *entries, by_name);
}

static void draw_media(void)
{
	frame("MEDIA", playing[0] ? "STOP" : NULL, NULL, NULL, NULL);
	text(12, 25 + 18 * ROWS + 6, GREY, 1, media_dir);
	if (!entries_n)
		centred(90, GREY, 2, "NOTHING TO PLAY");
	list(entries, entries_n, chosen, &top);
	if (playing[0]) {
		char s[64];

		snprintf(s, sizeof s, "PLAYING %s", playing);
		fill(0, H - 34, W, 16, BLACK);
		text(12, H - 32, GREEN, 1, s);
	}
}

static void media_open(int k)
{
	const char *e;
	int i;

	if (k < 0 || k >= entries_n)
		return;
	e = entries[k];
	if (e[strlen(e) - 1] == '/') {
		size_t n = strlen(media_dir);

		snprintf(media_dir + n, sizeof media_dir - n, "/%.*s", (int)strlen(e) - 1, e);
		chosen = top = 0;
		media_read();
		return;
	}
	/* The files of this directory from here on, one after the other. */
	for (i = 0; i < queue_n; i++)
		free(queue[i]);
	free(queue);
	queue = NULL;
	queue_n = 0;
	snprintf(queue_dir, sizeof queue_dir, "%s", media_dir);
	for (i = 0; i < entries_n; i++)
		if (entries[i][strlen(entries[i]) - 1] != '/') {
			queue = realloc(queue, sizeof *queue * (queue_n + 1));
			if (i == k)
				queue_at = queue_n;
			queue[queue_n++] = strdup(entries[i]);
		}
	play_at(queue_at);
}

static int media_up(void)
{
	char *slash;

	if (!strcmp(media_dir, "/media"))
		return 0;
	slash = strrchr(media_dir, '/');
	*slash = 0;
	chosen = top = 0;
	media_read();
	return 1;
}

/* CD */
static char *cd_items[] = { "Mount and browse", "Unmount", "Eject" };

static void draw_cd(void)
{
	frame("CD / DVD", NULL, NULL, NULL, NULL);
	list(cd_items, 3, chosen, &top);
	textf(12, 100, WHITE, 1, "DRIVE:  %s", access("/sys/block/sr0", F_OK) ? "NOT FOUND" : "PRESENT");
	textf(12, 116, WHITE, 1, "DISC:   %s", disc_in() ? "IN" : "NONE");
	textf(12, 132, WHITE, 1, "MOUNTED: %s", mounted("/media/cd") ? "/media/cd" : "NO");
}

static void cd_do(int k)
{
	if (k == 0) {
		if (disc_mount()) {
			say("NO DISC TO MOUNT");
			return;
		}
		snprintf(media_dir, sizeof media_dir, "/media/cd");
		page = MEDIA;
		chosen = top = 0;
		media_read();
	} else if (k == 1) {
		stop_playing();
		umount2("/media/cd", MNT_DETACH);
		say("UNMOUNTED");
	} else {
		stop_playing();
		umount2("/media/cd", MNT_DETACH);
		write_file("/sys/kernel/rnse/eject", "1");
		say("EJECTING");
	}
}

/* CAN */
static int can_fd = -1;
static struct can_frame can_seen[ROWS];
static int can_seen_n;
static long can_count;

static void draw_can(void)
{
	char buf[32];
	int i, n;

	frame("CAN", "SEND 7F0", NULL, NULL, NULL);
	if (can_fd < 0) {
		centred(90, GREY, 2, "NO CAN0");
		return;
	}
	for (i = 0; i < can_seen_n; i++) {
		struct can_frame *f = &can_seen[i];
		char line[64];
		int k, p;

		p = snprintf(line, sizeof line, "%03X [%d]", f->can_id & CAN_SFF_MASK, f->can_dlc);
		for (k = 0; k < f->can_dlc; k++)
			p += snprintf(line + p, sizeof line - p, " %02X", f->data[k]);
		text(12, 28 + 16 * i, WHITE, 1, line);
	}
	n = read_file("/sys/class/net/can0/statistics/tx_errors", buf, sizeof buf);
	textf(12, H - 32, GREY, 1, "RECEIVED %ld   SEND ERRORS %s", can_count, n > 0 ? buf : "?");
}

static void can_read(void)
{
	struct can_frame f;

	while (can_fd >= 0 && read(can_fd, &f, sizeof f) == sizeof f) {
		can_count++;
		if (can_seen_n == ROWS) {
			memmove(can_seen, can_seen + 1, sizeof can_seen[0] * (ROWS - 1));
			can_seen_n--;
		}
		can_seen[can_seen_n++] = f;
	}
}

/* System */
static int healthy_said;
static char *system_items[] = { "Open / close the display", "Eject the disc", "Shell (INFO: back)" };

static void draw_system(void)
{
	char a[64], b[64], mode[16], guard[16];
	int i, y = 86;
	long up;

	frame("SYSTEM", NULL, NULL, NULL, NULL);
	list(system_items, 3, chosen, &top);
	read_file("/sys/kernel/rnse/mode", mode, sizeof mode);
	if (read_file("/sys/kernel/rnse/healthy", guard, sizeof guard) < 0)
		strcpy(guard, "?");
	textf(12, y, WHITE, 1, "STARTED FROM: %s   GUARD: %s%s", mode, guard,
	      healthy_said ? " (HEALTHY)" : "");
	y += 14;
	read_file("/proc/sys/kernel/osrelease", a, sizeof a);
	textf(12, y, WHITE, 1, "LINUX %s", a);
	y += 14;
	read_file("/proc/uptime", a, sizeof a);
	up = atol(a);
	textf(12, y, WHITE, 1, "UP %ld:%02ld:%02ld", up / 3600, up / 60 % 60, up % 60);
	y += 14;
	read_file("/sys/kernel/rnse/display", a, sizeof a);
	textf(12, y, WHITE, 1, "DISPLAY %s", a);
	y += 14;
	strcpy(b, "A/D");
	for (i = 0; i < 7; i++) {
		char p[64];

		snprintf(p, sizeof p, "/sys/bus/iio/devices/iio:device0/in_voltage%d_raw", i);
		if (read_file(p, a, sizeof a) > 0)
			snprintf(b + strlen(b), sizeof b - strlen(b), " %s", a);
	}
	text(12, y, WHITE, 1, b);
	y += 14;
	textf(12, y, WHITE, 1, "CARDS: SLOT 0 %s, SLOT 1 %s   DISC: %s",
	      access("/sys/block/rnsesd0", F_OK) ? "-" : "IN",
	      access("/sys/block/rnsesd1", F_OK) ? "-" : "IN", disc_in() ? "IN" : "-");
	y += 14;
	textf(12, y, WHITE, 1, "RADIO PROCESSOR: %s", radio_ok ? "ANSWERS" : "SILENT");
}

/* A line in the kernel's log (and so on the serial console): what the
 * tests and a bench watch for. */
static void note(const char *what)
{
	char line[80];

	snprintf(line, sizeof line, "rnse-gui: %s\n", what);
	write_file("/dev/kmsg", line);
}

/* The shell on the second console, until INFO. */
static void shell(void)
{
	struct input_event ev;
	struct pollfd p = { .fd = faceplate, .events = POLLIN };

	if (keyboard >= 0)
		ioctl(keyboard, EVIOCGRAB, 0);
	ioctl(0, KDSETMODE, KD_TEXT);
	ioctl(0, VT_ACTIVATE, 2);
	note("shell");
	for (;;) {
		if (poll(&p, 1, 1000) <= 0) {
			reap();
			continue;
		}
		if (read(faceplate, &ev, sizeof ev) == sizeof ev && ev.type == EV_KEY &&
		    ev.code == BTN_TRIGGER_HAPPY1 + K_INFO && ev.value == 1)
			break;
	}
	ioctl(0, VT_ACTIVATE, 1);
	ioctl(0, VT_WAITACTIVE, 1);
	ioctl(0, KDSETMODE, KD_GRAPHICS);
	if (keyboard >= 0)
		ioctl(keyboard, EVIOCGRAB, 1);
	note("back");
}

static void go(int to)
{
	page = to;
	chosen = top = 0;
	if (to == MEDIA)
		media_read();
	if (to == RADIO && radio_ok && !player)
		radio_source(RADIO_TUNER, volume);
	if (to == CAN && can_fd < 0) {
		can_up();
		can_fd = can_socket();
		if (can_fd >= 0)
			fcntl(can_fd, F_SETFL, O_NONBLOCK);
	}
}

static void draw(void)
{
	switch (page) {
	case HOME: draw_home(); break;
	case RADIO: draw_radio(); break;
	case MEDIA: draw_media(); break;
	case CD: draw_cd(); break;
	case CAN: draw_can(); break;
	case SYSTEM: draw_system(); break;
	}
	footer_status();
	present();
}

static int items(void)
{
	return page == HOME ? 6 : page == MEDIA ? entries_n : page == CD ? 3 : page == SYSTEM ? 3 : 0;
}

static void key(int k)
{
	switch (k) {
	case K_INFO: go(HOME); return;
	case K_RADIO: go(RADIO); return;
	case K_MEDIA: go(MEDIA); return;
	case K_CAR: go(CAN); return;
	case K_SETUP: go(SYSTEM); return;
	case K_RETURN:
		if (page == MEDIA && media_up())
			return;
		if (page != HOME)
			go(HOME);
		return;
	case K_SOFT_TL:
		if (page == MEDIA && player)
			stop_playing();
		if (page == CAN && can_fd >= 0) {
			struct can_frame f = { .can_id = 0x7f0, .can_dlc = 4, .data = { 'R', 'N', 'S', 'E' } };

			if (write(can_fd, &f, sizeof f) != sizeof f)
				say("CAN: NOT SENT");
		}
		return;
	case K_NEXT:
	case K_PREV:
		if (player || page == MEDIA) {
			if (player)
				play_at(queue_at + (k == K_NEXT ? 1 : -1));
		} else if (page == RADIO) {
			frequency += k == K_NEXT ? 20 : -20;
			if (frequency > 2160)
				frequency = 1750;
			if (frequency < 1750)
				frequency = 2160;
			tune_at = now_ms() + 300;
		}
		return;
	case K_SELECT:
		break;
	default:
		return;
	}
	/* SELECT */
	if (page == HOME) {
		if (chosen == 5)
			shell();
		else
			go((int[]){ RADIO, MEDIA, CD, CAN, SYSTEM }[chosen]);
	} else if (page == MEDIA) {
		media_open(chosen);
	} else if (page == CD) {
		cd_do(chosen);
	} else if (page == SYSTEM) {
		if (chosen == 0)
			write_file("/sys/kernel/rnse/display", "toggle");
		else if (chosen == 1)
			write_file("/sys/kernel/rnse/eject", "1");
		else
			shell();
	}
}

static void knob(int select, int vol)
{
	if (select) {
		if (page == RADIO) {
			frequency += 2 * select;
			if (frequency > 2160)
				frequency = 2160;
			if (frequency < 1750)
				frequency = 1750;
			tune_at = now_ms() + 300;
		} else {
			int n = items();

			chosen += select;
			if (chosen >= n)
				chosen = n - 1;
			if (chosen < 0)
				chosen = 0;
		}
	}
	if (vol) {
		/* The knob's direction bit means "quieter". */
		volume += vol;
		if (volume > 30)
			volume = 30;
		if (volume < 0)
			volume = 0;
		volume_shown = now_ms();
		if (radio_ok)
			radio_volume(volume);
		settings_save();
	}
}

static volatile sig_atomic_t quit;

static void on_signal(int s)
{
	quit = 1;
}

int gui_main(int argc, char **argv)
{
	long started, last_draw = 0, last_mount = 0;
	char mode[16] = "";
	int dirty = 1, from_flash;

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);
	fb_setup();
	font_setup();
	ioctl(0, KDSETMODE, KD_GRAPHICS);
	faceplate = find_input("RNS-E faceplate");
	keyboard = find_input("RNS-E rotary keyboard");
	if (keyboard >= 0)
		ioctl(keyboard, EVIOCGRAB, 1);
	settings_load();
	read_file("/sys/kernel/rnse/mode", mode, sizeof mode);
	from_flash = !strcmp(mode, "flash");

	memset(canvas, BLACK, sizeof canvas);
	centred(100, ORANGE, 2, "STARTING");
	present();
	radio_ok = radio_open() == 0 && radio_command(0x0b, NULL, 0, (uint8_t[64]){ 0 }, 64) >= 0;
	if (radio_ok && from_flash)
		radio_start();	/* from flash nobody has started it */
	if (radio_ok) {
		radio_source(RADIO_TUNER, volume);
		radio_tune(frequency);
	}
	started = now_ms();
	automount();
	note(radio_ok ? "ready" : "ready, the radio processor is silent");

	while (!quit) {
		struct pollfd p[2] = { { .fd = faceplate, .events = POLLIN }, { .fd = can_fd, .events = POLLIN } };
		struct input_event ev;

		if (dirty || now_ms() - last_draw > 1000) {
			draw();
			last_draw = now_ms();
			dirty = 0;
		}
		if (from_flash && !healthy_said && now_ms() - started > 10000) {
			healthy_said = write_file("/sys/kernel/rnse/healthy", "1") == 0;
			dirty = 1;
		}
		if (poll(p, can_fd >= 0 ? 2 : 1, 200) > 0) {
			while (faceplate >= 0 && read(faceplate, &ev, sizeof ev) == sizeof ev) {
				if (ev.type == EV_KEY && ev.value == 1 &&
				    ev.code >= BTN_TRIGGER_HAPPY1 && ev.code <= BTN_TRIGGER_HAPPY1 + K_CD_EJECT)
					key(ev.code - BTN_TRIGGER_HAPPY1);
				else if (ev.type == EV_REL && ev.code == REL_DIAL)
					knob(ev.value, 0);
				else if (ev.type == EV_REL && ev.code == REL_MISC)
					knob(0, ev.value);
				dirty = 1;
			}
			if (p[1].revents & POLLIN) {
				can_read();
				dirty = 1;
			}
		}
		reap();
		if (page == RADIO)
			radio_poll();
		if (now_ms() - last_mount > 1000) {
			automount();
			last_mount = now_ms();
		}
	}
	stop_playing();
	ioctl(0, KDSETMODE, KD_TEXT);
	return 0;
}
