/*
 * FileRead — minimal PPC file I/O test tool.
 */

#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
	FILE *fp;
	char buf[64];
	size_t n;

	if (argc < 2) return 1;

	fp = fopen(argv[1], "r");
	if (!fp) return 2;

	/* This fprintf to stderr corrupts fp? */
	fprintf(stderr, "fopen ok\n");

	memset(buf, 0, sizeof(buf));
	n = fread(buf, 1, 20, fp);

	fprintf(stderr, "fread: %d bytes\n", (int)n);
	fprintf(stderr, "data: %.20s\n", buf);

	fclose(fp);
	return 0;
}
