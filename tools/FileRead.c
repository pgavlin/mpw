/*
 * FileRead — minimal PPC file I/O test tool.
 * Tests both stdio (fopen/fread) and low-level (open/read) I/O.
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

int main(int argc, char *argv[])
{
	int fd, n;
	char buf[64];

	if (argc < 2) {
		fprintf(stderr, "Usage: FileRead <file>\n");
		return 1;
	}

	/* Test low-level open/read first */
	fd = open(argv[1], O_RDONLY);
	fprintf(stdout, "open: fd=%d\n", fd);

	if (fd < 0) {
		fprintf(stderr, "open failed\n");
		return 2;
	}

	memset(buf, 0, sizeof(buf));
	n = read(fd, buf, 16);
	fprintf(stdout, "read: n=%d\n", n);

	if (n > 0) {
		buf[n] = 0;
		fprintf(stdout, "data: %s\n", buf);
	}

	close(fd);
	return 0;
}
