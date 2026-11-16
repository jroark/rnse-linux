/* rnse-can: the infotainment bus through SocketCAN (can0, the
 * rnse-hcan2 driver, 100 kbit/s).
 *
 *   rnse-can up                bring can0 up (BusyBox's ip cannot set
 *                              a CAN bit rate; the driver's default is
 *                              the bus's)
 *   rnse-can send ID#HEX ...   send frames, e.g. 7f0#0102
 *   rnse-can dump [N]          print frames as they come, N of them
 *
 * On a bench with no other node nothing acknowledges a frame: the
 * driver gives each up after 100 ms (a transmit error). */
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include "rnse.h"

int can_up(void)
{
	struct ifreq ifr;
	int s = socket(PF_CAN, SOCK_RAW, CAN_RAW), r;

	if (s < 0)
		return -1;
	memset(&ifr, 0, sizeof ifr);
	strcpy(ifr.ifr_name, "can0");
	r = ioctl(s, SIOCGIFFLAGS, &ifr);
	if (!r && !(ifr.ifr_flags & IFF_UP)) {
		ifr.ifr_flags |= IFF_UP;
		r = ioctl(s, SIOCSIFFLAGS, &ifr);
	}
	close(s);
	return r;
}

int can_socket(void)
{
	struct sockaddr_can a;
	struct ifreq ifr;
	int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);

	if (s < 0)
		return -1;
	memset(&ifr, 0, sizeof ifr);
	strcpy(ifr.ifr_name, "can0");
	if (ioctl(s, SIOCGIFINDEX, &ifr))
		goto fail;
	memset(&a, 0, sizeof a);
	a.can_family = AF_CAN;
	a.can_ifindex = ifr.ifr_ifindex;
	if (bind(s, (struct sockaddr *)&a, sizeof a))
		goto fail;
	return s;
fail:
	close(s);
	return -1;
}

static int parse(const char *text, struct can_frame *f)
{
	char *end;
	unsigned long id = strtoul(text, &end, 16);
	int n = 0;

	if (*end != '#' || id > 0x7ff)
		return -1;
	memset(f, 0, sizeof *f);
	f->can_id = id;
	for (end++; end[0] && end[1] && n < 8; end += 2) {
		char b[3] = { end[0], end[1], 0 };

		f->data[n++] = strtoul(b, NULL, 16);
	}
	f->can_dlc = n;
	return 0;
}

int can_main(int argc, char **argv)
{
	struct can_frame f;
	int s, i, n;

	if (argc < 2) {
		fprintf(stderr, "usage: rnse-can up | send ID#HEX... | dump [N]\n");
		return 2;
	}
	if (can_up()) {
		perror("rnse-can: can0 up");
		return 1;
	}
	if (!strcmp(argv[1], "up"))
		return 0;
	s = can_socket();
	if (s < 0) {
		perror("rnse-can: can0");
		return 1;
	}
	if (!strcmp(argv[1], "send")) {
		for (i = 2; i < argc; i++) {
			if (parse(argv[i], &f)) {
				fprintf(stderr, "rnse-can: %s: not ID#HEX\n", argv[i]);
				return 2;
			}
			if (write(s, &f, sizeof f) != sizeof f) {
				perror("rnse-can: send");
				return 1;
			}
		}
		return 0;
	}
	if (!strcmp(argv[1], "dump")) {
		long count = argc > 2 ? atol(argv[2]) : -1;

		while (count != 0 && read(s, &f, sizeof f) == sizeof f) {
			printf("%03x [%d]", f.can_id & CAN_SFF_MASK, f.can_dlc);
			for (n = 0; n < f.can_dlc; n++)
				printf(" %02x", f.data[n]);
			printf("\n");
			fflush(stdout);
			if (count > 0)
				count--;
		}
		return 0;
	}
	fprintf(stderr, "rnse-can: %s?\n", argv[1]);
	return 2;
}
