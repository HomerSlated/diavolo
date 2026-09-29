/*
 * Single-thread BLAKE3 throughput on an in-memory buffer (touched before
 * timing), three ways, each the median of REPS runs:
 *   tree  1 MiB pieces hashed in shuffled order, then b3_root_of_pieces
 *   root  b3_hash over the whole buffer (our tree layer, in order)
 *   ref   the system libblake3's public hasher (upstream C tree layer)
 * All three roots must agree. Usage: b3speed [MiB [REPS]]
 */
#include <blake3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "b3tree.h"

#define MIB (1u << 20)

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
	size_t mib = argc > 1 ? strtoul(argv[1], 0, 10) : 1024;
	int reps = argc > 2 ? atoi(argv[2]) : 7;
	size_t len = mib * MIB - 777;	/* a short last piece, as real files have */
	size_t np = (len + MIB - 1) / MIB;
	uint8_t *buf = malloc(len);
	uint8_t (*cv)[B3_OUT] = malloc(np * B3_OUT);
	size_t *order = malloc(np * sizeof *order);
	uint64_t x = 0x9E3779B97F4A7C15u;
	for (size_t i = 0; i < len; i++) {	/* also faults every page in */
		x ^= x << 13, x ^= x >> 7, x ^= x << 17;
		buf[i] = (uint8_t)(x >> 32);
	}
	for (size_t i = 0; i < np; i++)
		order[i] = i;
	for (size_t i = np; i-- > 1;) {
		x ^= x << 13, x ^= x >> 7, x ^= x << 17;
		size_t j = x % (i + 1), t = order[i];
		order[i] = order[j];
		order[j] = t;
	}
	double t[3][64];
	uint8_t h[3][B3_OUT];
	if (reps > 64)
		reps = 64;
	for (int r = 0; r < reps; r++) {
		double t0 = now();
		for (size_t k = 0; k < np; k++) {
			size_t i = order[k], off = i * MIB;
			b3_subtree(buf + off, len - off < MIB ? len - off : MIB, off, cv[i]);
		}
		b3_root_of_pieces((const uint8_t (*)[B3_OUT])cv, np, h[0]);
		double t1 = now();
		b3_hash(buf, len, h[1]);
		double t2 = now();
		blake3_hasher bh;
		blake3_hasher_init(&bh);
		blake3_hasher_update(&bh, buf, len);
		blake3_hasher_finalize(&bh, h[2], B3_OUT);
		double t3 = now();
		t[0][r] = t1 - t0, t[1][r] = t2 - t1, t[2][r] = t3 - t2;
	}
	if (memcmp(h[0], h[1], B3_OUT) || memcmp(h[1], h[2], B3_OUT)) {
		fprintf(stderr, "b3speed: roots disagree\n");
		return 1;
	}
	static const char *name[3] = { "tree", "root", "ref" };
	printf("kernel: %s, %zu MiB, %d reps, median\n", b3_kernel(), mib, reps);
	for (int m = 0; m < 3; m++) {
		qsort(t[m], reps, sizeof(double), cmp_d);
		printf("  %-4s %6.3f s  %5.2f GiB/s\n", name[m], t[m][reps / 2],
		       len / t[m][reps / 2] / (1u << 30));
	}
	return 0;
}
