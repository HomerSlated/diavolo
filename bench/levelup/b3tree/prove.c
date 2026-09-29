/*
 * C port of the advisor's BLAKE3 tree-merge proof (report P1.1, Appendix B).
 *
 * BLAKE3 of a file's logical bytes (holes as zeros), computed from per-piece
 * CVs that arrive in ANY order, then merged in tree order. Pieces are cut as
 * a disk-order reader would cut them: arbitrary 4 KiB-aligned "extents", each
 * decomposed into maximal aligned power-of-two runs capped at 1 MiB.
 *
 * The reference is the system libblake3's own hasher, which shares no code
 * with b3tree.c (our vendored kernels are renamed; see the Makefile).
 * Usage: b3prove FILE   -- prints "N boundary cases OK", then two timed
 * whole-file lines whose root must equal `b3sum FILE`.
 */
#include <blake3.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "b3tree.h"

#define CAP (1u << 20)

typedef struct {
	uint64_t off, len;
	uint8_t cv[B3_OUT];
} piece;

static piece *g_p;
static size_t g_np;

static void die(const char *m)
{
	fprintf(stderr, "b3prove: %s\n", m);
	exit(1);
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static void zero_cv(uint64_t off, uint64_t len, uint8_t cv[B3_OUT])
{
	static const uint8_t Z[CAP];
	if (len > CAP) {	/* a long hole: its own subtrees, merged */
		uint8_t l[B3_OUT], r[B3_OUT];
		uint64_t ll = b3_left_len(len);
		zero_cv(off, ll, l);
		zero_cv(off + ll, len - ll, r);
		b3_parent(l, r, cv);
	} else {
		b3_subtree(Z, len, off, cv);
	}
}

/* first stored piece with offset >= off (pieces are sorted by offset) */
static size_t lower(uint64_t off)
{
	size_t lo = 0, hi = g_np;
	while (lo < hi) {
		size_t m = (lo + hi) / 2;
		if (g_p[m].off < off)
			lo = m + 1;
		else
			hi = m;
	}
	return lo;
}

static void node(uint64_t off, uint64_t len, uint8_t cv[B3_OUT])
{
	size_t i = lower(off);
	if (i < g_np && g_p[i].off == off && g_p[i].len == len) {
		memcpy(cv, g_p[i].cv, B3_OUT);
		return;
	}
	size_t j = lower(off + len);	/* any stored piece intersecting [off, off+len)? */
	int covered = (j > 0 && g_p[j - 1].off + g_p[j - 1].len > off);
	if (!covered) {
		zero_cv(off, len, cv);	/* a hole: hash zeros at this offset */
		return;
	}
	if (len <= B3_CHUNK)
		die("stored piece does not align with the tree");
	uint8_t l[B3_OUT], r[B3_OUT];
	uint64_t ll = b3_left_len(len);
	node(off, ll, l);
	node(off + ll, len - ll, r);
	b3_parent(l, r, cv);
}

static uint64_t g_rng;
static uint64_t rng(void)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 7;
	g_rng ^= g_rng << 17;
	return g_rng;
}

static int cmp_off(const void *a, const void *b)
{
	uint64_t x = ((const piece *)a)->off, y = ((const piece *)b)->off;
	return (x > y) - (x < y);
}

static void hex(const uint8_t h[B3_OUT], char s[2 * B3_OUT + 1])
{
	for (unsigned i = 0; i < B3_OUT; i++)
		sprintf(s + 2 * i, "%02x", h[i]);
}

static void run(uint8_t *data, uint64_t total, uint64_t seed, int holes, int timing)
{
	g_rng = seed | 1;
	/* cut [0,total) into "extents" at random 4 KiB boundaries; mark some as holes */
	size_t nc = 1, cc = 64;
	uint64_t *cuts = malloc(cc * sizeof *cuts);
	cuts[0] = 0;
	while (cuts[nc - 1] < total) {
		uint64_t step = (rng() % 3000 + 1) * 4096;	/* 4 KiB .. ~12 MiB */
		uint64_t c = cuts[nc - 1] + step;
		if (nc == cc)
			cuts = realloc(cuts, (cc *= 2) * sizeof *cuts);
		cuts[nc++] = c < total ? c : total;
	}
	size_t cap = 1024;
	g_np = 0;
	g_p = malloc(cap * sizeof *g_p);
	for (size_t w = 0; w + 1 < nc; w++) {
		uint64_t a = cuts[w], b = cuts[w + 1];
		if (holes && rng() % 5 == 0) {
			memset(data + a, 0, b - a);
			continue;
		}
		while (a < b) {	/* aligned power-of-two runs, >= 1 KiB, <= CAP */
			uint64_t sz = CAP;
			while (sz > B3_CHUNK && (a % sz || a + sz > b))
				sz /= 2;
			uint64_t len = sz < b - a ? sz : b - a;	/* the file's last chunk may be short */
			if (g_np == cap)
				g_p = realloc(g_p, (cap *= 2) * sizeof *g_p);
			g_p[g_np++] = (piece){ .off = a, .len = len };
			a += len;
		}
	}
	free(cuts);
	/* arrival order: shuffled, as a disk-order reader of a fragmented file would see it */
	for (size_t i = g_np; i-- > 1;) {
		size_t j = rng() % (i + 1);
		piece t = g_p[i];
		g_p[i] = g_p[j];
		g_p[j] = t;
	}

	double t0 = now();
	uint8_t got[B3_OUT];
	int whole = 0;
	for (size_t i = 0; i < g_np; i++) {
		if (g_p[i].off == 0 && g_p[i].len == total) {	/* the piece IS the file: root */
			b3_hash(data, total, got);
			whole = 1;
		} else {
			b3_subtree(data + g_p[i].off, g_p[i].len, g_p[i].off, g_p[i].cv);
		}
	}
	double t1 = now();
	qsort(g_p, g_np, sizeof *g_p, cmp_off);
	if (whole) {
		/* got already set */
	} else if (total <= B3_CHUNK) {
		b3_hash(data, total, got);	/* empty, or one chunk that is all hole */
	} else {
		uint8_t l[B3_OUT], r[B3_OUT];
		uint64_t ll = b3_left_len(total);
		node(0, ll, l);
		node(ll, total - ll, r);
		b3_root_parent(l, r, got);
	}
	double t2 = now();
	uint8_t want[B3_OUT];
	blake3_hasher h;
	blake3_hasher_init(&h);
	blake3_hasher_update(&h, data, total);
	blake3_hasher_finalize(&h, want, B3_OUT);
	double t3 = now();
	char gs[65], ws[65];
	hex(got, gs);
	hex(want, ws);
	if (memcmp(got, want, B3_OUT)) {
		fprintf(stderr, "MISMATCH total=%lu seed=%lu holes=%d\n  got  %s\n  want %s\n",
			(unsigned long)total, (unsigned long)seed, holes, gs, ws);
		exit(1);
	}
	if (timing)
		printf("total=%lu pieces=%zu (shuffled) holes=%d: piece CVs %.3fs, merge %.4fs, "
		       "reference libblake3 %.3fs, cv-map ~%zu KiB  OK %s\n",
		       (unsigned long)total, g_np, holes, t1 - t0, t2 - t1, t3 - t2,
		       g_np * sizeof *g_p / 1024, gs);
	free(g_p);
}

int main(int argc, char **argv)
{
	if (argc != 2)
		die("usage: b3prove FILE");
	int fd = open(argv[1], O_RDONLY);
	struct stat st;
	if (fd < 0 || fstat(fd, &st))
		die("cannot open file");
	uint64_t n = (uint64_t)st.st_size;
	uint8_t *orig = malloc(n + 1), *d = malloc(n + 1);
	for (uint64_t got = 0; got < n;) {
		ssize_t r = read(fd, orig + got, n - got);
		if (r <= 0)
			die("read failed");
		got += (uint64_t)r;
	}
	close(fd);
	printf("kernel: %s\n", b3_kernel());
	static const uint64_t T[] = { 0, 1, 1023, 1024, 1025, 4096, 12345, 65536,
				      (1 << 20) - 1, 1 << 20, (1 << 20) + 1,
				      3 * (1 << 20) + 12345, 7 * (1 << 20) + 4096,
				      100 * (1 << 20) + 777 };
	int cases = 0;
	for (size_t k = 0; k < sizeof T / sizeof *T; k++) {
		if (T[k] > n)
			continue;
		for (uint64_t seed = 1; seed <= 8; seed++)
			for (int holes = 0; holes <= 1; holes++) {
				memcpy(d, orig, T[k]);
				run(d, T[k], seed * 7919 + T[k], holes, 0);
				cases++;
			}
	}
	printf("%d boundary cases OK\n", cases);
	memcpy(d, orig, n);
	run(d, n, 42, 0, 1);
	memcpy(d, orig, n);
	run(d, n, 43, 1, 1);
	return 0;
}
