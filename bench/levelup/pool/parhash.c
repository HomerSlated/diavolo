/*
 * Multi-threaded BLAKE3 of an in-memory buffer as 1 MiB pieces, via pool:
 * the pieces' CVs in parallel (any order), then the root from them. Each
 * thread count is the median of REPS; every root must equal b3_hash's.
 * Usage: parhash [MiB [REPS [THREADS...]]]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../b3tree/b3tree.h"
#include "pool.h"

#define MIB (1u << 20)

typedef struct {
	const uint8_t *buf;
	size_t len;
	uint8_t (*cv)[B3_OUT];
} job;

static void piece(void *ctx, size_t i)
{
	const job *j = ctx;
	size_t off = i * MIB;
	b3_subtree(j->buf + off, j->len - off < MIB ? j->len - off : MIB, off, j->cv[i]);
}

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
	static const unsigned DEF[] = { 1, 2, 4, 6, 8, 12 };
	size_t len = mib * MIB - 777, np = (len + MIB - 1) / MIB;
	uint8_t *buf = malloc(len), want[B3_OUT], got[B3_OUT];
	job j = { buf, len, malloc(np * B3_OUT) };
	uint64_t x = 0x9E3779B97F4A7C15u;
	for (size_t i = 0; i < len; i++) {
		x ^= x << 13, x ^= x >> 7, x ^= x << 17;
		buf[i] = (uint8_t)(x >> 32);
	}
	b3_hash(buf, len, want);
	if (reps > 64)
		reps = 64;
	printf("C pool, kernel %s, %zu MiB, %d reps, median\n", b3_kernel(), mib, reps);
	int nt = argc > 3 ? argc - 3 : (int)(sizeof DEF / sizeof *DEF);
	for (int k = 0; k < nt; k++) {
		unsigned threads = argc > 3 ? (unsigned)atoi(argv[3 + k]) : DEF[k];
		pool *p = pool_new(threads);
		double t[64];
		for (int r = 0; r < reps; r++) {
			memset(got, 0, sizeof got);
			double t0 = now();
			pool_for(p, np, piece, &j);
			b3_root_of_pieces((const uint8_t (*)[B3_OUT])j.cv, np, got);
			t[r] = now() - t0;
			if (memcmp(got, want, B3_OUT)) {
				fprintf(stderr, "parhash: wrong root with %u threads\n", threads);
				return 1;
			}
		}
		pool_free(p);
		qsort(t, reps, sizeof *t, cmp_d);
		printf("  %2u threads  %6.4f s  %6.2f GiB/s\n", threads, t[reps / 2],
		       len / t[reps / 2] / (1u << 30));
	}
	return 0;
}
