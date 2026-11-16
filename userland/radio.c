/* The radio processor (tuner, amplifier, power) on /dev/ttyRN1: the
 * protocol of sdk/src/radio.c and docs/hardware.md, "Radio/audio
 * processor".  Frames: 0x01 from us (0x00 from it), a command, the
 * length of the data in 16 bits, the data, the complement of the sum.
 * A request is answered by a frame of the same command, a setting by
 * command 0x20 with one byte, 0 for done; any other byte is a refusal
 * (busy, it seems: a unit takes the frame when it comes again 100 ms
 * later).  The line: 9600 bit/s, eight bits, no parity. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "rnse.h"

static int fd = -1;

int radio_open(void)
{
	struct termios t;

	if (fd >= 0)
		return 0;
	fd = open("/dev/ttyRN1", O_RDWR | O_NOCTTY);
	if (fd < 0)
		return -1;
	tcgetattr(fd, &t);
	cfmakeraw(&t);
	cfsetispeed(&t, B9600);
	cfsetospeed(&t, B9600);
	t.c_cflag |= CLOCAL | CREAD;
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	tcsetattr(fd, TCSANOW, &t);
	tcflush(fd, TCIOFLUSH);
	return 0;
}

static int getc_for(int ms)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	uint8_t c;

	if (poll(&p, 1, ms) <= 0)
		return -1;
	if (read(fd, &c, 1) != 1)
		return -1;
	return c;
}

/* One frame from the radio processor: its command, its data. */
static int frame(uint8_t *data, int max, int *n, int ms)
{
	int c, cmd, len, i;
	uint8_t sum;

	do {
		c = getc_for(ms);
		if (c < 0)
			return -1;
	} while (c != 0x00);
	cmd = getc_for(50);
	len = getc_for(50);
	c = getc_for(50);
	if (cmd < 0 || len < 0 || c < 0)
		return -1;
	len = len << 8 | c;
	if (len > 300)
		return -1;
	sum = cmd + (len >> 8) + len;
	for (i = 0; i < len; i++) {
		c = getc_for(50);
		if (c < 0)
			return -1;
		sum += c;
		if (i < max)
			data[i] = c;
	}
	c = getc_for(50);
	if (c < 0 || (uint8_t)c != (uint8_t)~sum)
		return -1;
	*n = len < max ? len : max;
	return cmd;
}

static int patience = 600, tries = 3;

int radio_command(int cmd, const uint8_t *data, int n, uint8_t *answer, int max)
{
	int try, silent = 0, len = 0, got;
	uint8_t buf[320], sum;
	long start;

	if (radio_open())
		return -1;
	if (n > 300)
		return -1;
	buf[0] = 0x01;
	buf[1] = cmd;
	buf[2] = n >> 8;
	buf[3] = n;
	memcpy(buf + 4, data, n);
	sum = 0x01 + cmd + (n >> 8) + n;
	for (int i = 0; i < n; i++)
		sum += data[i];
	buf[4 + n] = ~sum;
	for (try = 0; silent < tries && try < 20; try++) {
		tcflush(fd, TCIFLUSH);
		if (write(fd, buf, 5 + n) != 5 + n)
			return -1;
		start = now_ms();
		got = -1;
		while (now_ms() - start < patience) {
			got = frame(answer, max, &len, patience - (now_ms() - start));
			if (got == cmd || (got == 0x20 && cmd != 0x20))
				break;
		}
		if (now_ms() - start >= patience && got != cmd && got != 0x20) {
			silent++;
			continue;
		}
		if (!(got == 0x20 && len && answer[0]))
			return len;
		usleep(100000);	/* refused: busy; again in a moment */
	}
	return silent < tries ? -2 : -1;
}

int radio_source(int source, int volume)
{
	uint8_t d[9] = { 0x08, source, volume, 0, 0, 0, 0, 0x01, 0x03 }, a[4];

	return radio_command(0x81, d, 9, a, sizeof a) < 0 ? -1 : 0;
}

int radio_volume(int volume)
{
	uint8_t d[2] = { 0x01, volume }, a[4];

	return radio_command(0x19, d, 2, a, sizeof a) < 0 ? -1 : 0;
}

int radio_tune(int frequency)
{
	uint8_t d[4] = { 0x03, 0x02, frequency >> 8, frequency }, a[8];

	return radio_command(0x82, d, 4, a, sizeof a) < 0 ? -1 : 0;
}

int radio_status(uint8_t *status, int max)
{
	return radio_command(0x88, NULL, 0, status, max);
}

/* The frames the stock software sends at every start, in its order
 * (sdk/src/radio.c radio_start): for a radio processor that the boot
 * ROM left as it was, Linux being started from flash.  Harmless after
 * the stock software has sent them.  The number not taken. */
int radio_start(void)
{
	static const uint8_t frames[][5] = {
		{ 0x80, 2, 0x00, 0x02 }, { 0xdd, 0 }, { 0x0b, 0 }, { 0xd1, 0 }, { 0x88, 0 }, { 0x8a, 0 },
		{ 0x80, 2, 0x02, 0x01 }, { 0x17, 1, 0x01 }, { 0x19, 3, 0x07, 0x03, 0x0a }, { 0xd1, 0 },
		{ 0x19, 3, 0x07, 0x00, 0x17 }, { 0xd9, 0 },
	};
	static const uint8_t later[][5] = {
		{ 0x81, 2, 0x0a, 0x0d }, { 0xd0, 0 }, { 0x81, 1, 0x06 }, { 0xde, 1, 0x01 },
		{ 0x80, 2, 0x08, 0x00 }, { 0x81, 2, 0x0a, 0x02 },
	};
	uint8_t answer[64];
	unsigned i;
	int silent = 0, r;

	for (i = 0; i < sizeof frames / sizeof frames[0]; i++) {
		/* 0x8a: a unit's radio processor says nothing to it. */
		if (frames[i][0] == 0x8a)
			patience = 100, tries = 1;
		r = radio_command(frames[i][0], frames[i] + 2, frames[i][1], answer, sizeof answer);
		if (frames[i][0] == 0x8a)
			patience = 600, tries = 3, r = 0;
		if (r < 0)
			silent++;
		if (i == 1 && silent == 2)
			return 99;	/* nobody there */
	}
	if (radio_source(RADIO_TUNER, 7))
		silent++;
	for (i = 0; i < sizeof later / sizeof later[0]; i++)
		if (radio_command(later[i][0], later[i] + 2, later[i][1], answer, sizeof answer) < 0)
			silent++;
	return silent;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: rnse-radio start | status | version\n"
		"       rnse-radio source tuner|decoder|cd [VOLUME]\n"
		"       rnse-radio volume 0-30 | tune MHZ (87.5-108)\n");
}

int radio_main(int argc, char **argv)
{
	uint8_t a[64];
	int n, i;

	if (argc < 2) {
		usage();
		return 2;
	}
	if (radio_open()) {
		perror("/dev/ttyRN1");
		return 1;
	}
	if (!strcmp(argv[1], "start")) {
		n = radio_start();
		printf("radio processor: %s (%d frames not taken)\n", n == 99 ? "silent" : "started", n);
		return n == 99;
	}
	if (!strcmp(argv[1], "version")) {
		n = radio_command(0x0b, NULL, 0, a, sizeof a);
		if (n < 0)
			return 1;
		printf("%.*s\n", n < 16 ? n : 16, a);
		return 0;
	}
	if (!strcmp(argv[1], "status")) {
		n = radio_status(a, sizeof a);
		if (n < 0)
			return 1;
		if (n >= 35)
			printf("frequency %d.%d MHz, text \"%.8s\", source %d, volume %d\n",
			       (a[4] << 8 | a[5]) / 20, (a[4] << 8 | a[5]) % 20 * 5 / 10, a + 8, a[33], a[34]);
		for (i = 0; i < n; i++)
			printf("%02x%s", a[i], i % 16 == 15 || i == n - 1 ? "\n" : " ");
		return 0;
	}
	if (!strcmp(argv[1], "source") && argc >= 3) {
		int s = !strcmp(argv[2], "tuner") ? RADIO_TUNER : !strcmp(argv[2], "decoder") ? RADIO_DECODER :
			!strcmp(argv[2], "cd") ? RADIO_CD : atoi(argv[2]);

		return radio_source(s, argc > 3 ? atoi(argv[3]) : 7) ? 1 : 0;
	}
	if (!strcmp(argv[1], "volume") && argc >= 3)
		return radio_volume(atoi(argv[2])) ? 1 : 0;
	if (!strcmp(argv[1], "tune") && argc >= 3)
		return radio_tune((int)(atof(argv[2]) * 20 + 0.5)) ? 1 : 0;
	usage();
	return 2;
}
