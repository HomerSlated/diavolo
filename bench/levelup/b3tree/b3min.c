/* Minimal b3sum (one file, one thread) via b3tree: the binary-size probe. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "b3tree.h"

#define MIB (1u << 20)

static uint8_t buf[MIB];

int main(int argc, char **argv)
{
	int fd = argc == 2 ? open(argv[1], O_RDONLY) : -1;
	if (fd < 0)
		return 2;
	uint8_t (*cv)[B3_OUT] = NULL, root[B3_OUT];
	size_t np = 0;
	for (;;) {
		size_t n = 0;
		ssize_t r;
		while (n < MIB && (r = read(fd, buf + n, MIB - n)) > 0)
			n += (size_t)r;
		if (np == 0 && n < MIB) {	/* the whole file is one piece: it is the root */
			b3_hash(buf, n, root);
			break;
		}
		if (n == 0) {
			b3_root_of_pieces((const uint8_t (*)[B3_OUT])cv, np, root);
			break;
		}
		if (!(np & (np - 1)) && !(cv = realloc(cv, 2 * (np ? np : 1) * B3_OUT)))
			return 1;
		b3_subtree(buf, n, (uint64_t)np * MIB, cv[np]);
		np++;
		if (n < MIB) {
			b3_root_of_pieces((const uint8_t (*)[B3_OUT])cv, np, root);
			break;
		}
	}
	for (unsigned i = 0; i < B3_OUT; i++)
		printf("%02x", root[i]);
	printf("  %s\n", argv[1]);
	return 0;
}
