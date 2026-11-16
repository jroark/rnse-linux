/* rnse: one program, called by one of its names. */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "rnse.h"

int read_file(const char *path, char *buf, int max)
{
	int fd = open(path, O_RDONLY), n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, max - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = 0;
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		buf[--n] = 0;
	return n;
}

int write_file(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY), r;

	if (fd < 0)
		return -1;
	r = write(fd, text, strlen(text));
	close(fd);
	return r < 0 ? -1 : 0;
}

long now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

static const struct {
	const char *name;
	int (*main)(int, char **);
} parts[] = {
	{ "rnse-radio", radio_main },
	{ "rnse-play", play_main },
	{ "rnse-can", can_main },
	{ "rnse-gui", gui_main },
};

int main(int argc, char **argv)
{
	const char *name = strrchr(argv[0], '/');
	unsigned i;

	name = name ? name + 1 : argv[0];
	if (!strcmp(name, "rnse") && argc > 1) {
		argv++;
		argc--;
		name = argv[0];
	}
	for (i = 0; i < sizeof parts / sizeof parts[0]; i++)
		if (!strcmp(name, parts[i].name) || !strcmp(name, parts[i].name + 5))
			return parts[i].main(argc, argv);
	fprintf(stderr, "usage: rnse radio|play|can|gui ...\n");
	return 2;
}
