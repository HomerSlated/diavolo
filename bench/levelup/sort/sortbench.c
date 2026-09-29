/*
 * The read plan's sort, in isolation: accbench rawsort's piece_t array
 * (dumped with ACCBENCH_PLAN_DUMP=FILE, unsorted, as the plan is built)
 * sorted by pblk by each candidate. Each is timed on a fresh copy, median of
 * REPS, and must produce exactly glibc qsort's result (pblk is unique on
 * ext4, so there is only one correct order).
 *
 *   qsort    glibc qsort with accbench's comparator (the baseline)
 *   intro    our introsort: comparator inlined, insertion sort below 16,
 *            ninther pivots, heapsort if recursion goes bad
 *   radix    our LSD radix sort on pblk: pass count and digit width from the
 *            largest key, one histogram scan for all passes, constant digits
 *            skipped, and an O(n) already-sorted check first
 *   pack     radix on 64-bit (pblk << 24 | index) words, then one gather:
 *            moves 8 bytes per element per pass instead of 24
 *
 * Usage: sortbench PLAN... [-r REPS]
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {	/* as in accbench.c */
	uint64_t pblk;
	uint32_t nblk, file, ext, off;
} piece_t;

/* ---- glibc qsort ---- */

static int cmp_piece(const void *a, const void *b)
{
	uint64_t x = ((const piece_t *)a)->pblk, y = ((const piece_t *)b)->pblk;
	return (x > y) - (x < y);
}

static void s_qsort(piece_t *a, size_t n)
{
	qsort(a, n, sizeof *a, cmp_piece);
}

/* ---- introsort ---- */

#define SWAP(x, y) do { piece_t t_ = (x); (x) = (y); (y) = t_; } while (0)

static void insertion(piece_t *a, size_t n)
{
	for (size_t i = 1; i < n; i++) {
		piece_t x = a[i];
		size_t j = i;
		for (; j > 0 && a[j - 1].pblk > x.pblk; j--)
			a[j] = a[j - 1];
		a[j] = x;
	}
}

static void sift(piece_t *a, size_t i, size_t n)
{
	piece_t x = a[i];
	for (size_t c; (c = 2 * i + 1) < n; i = c) {
		if (c + 1 < n && a[c + 1].pblk > a[c].pblk)
			c++;
		if (a[c].pblk <= x.pblk)
			break;
		a[i] = a[c];
	}
	a[i] = x;
}

static void heapsort_(piece_t *a, size_t n)
{
	for (size_t i = n / 2; i-- > 0;)
		sift(a, i, n);
	for (size_t i = n; i-- > 1;) {
		SWAP(a[0], a[i]);
		sift(a, 0, i);
	}
}

static size_t med3(const piece_t *a, size_t i, size_t j, size_t k)
{
	uint64_t x = a[i].pblk, y = a[j].pblk, z = a[k].pblk;
	return x < y ? (y < z ? j : x < z ? k : i) : (x < z ? i : y < z ? k : j);
}

static void intro(piece_t *a, size_t n, int depth)
{
	while (n > 16) {
		if (depth-- == 0) {
			heapsort_(a, n);
			return;
		}
		size_t m = n / 2, p;
		if (n > 128) {	/* Tukey's ninther */
			size_t s = n / 8;
			p = med3(a, med3(a, 0, s, 2 * s), med3(a, m - s, m, m + s),
				 med3(a, n - 1 - 2 * s, n - 1 - s, n - 1));
		} else {
			p = med3(a, 0, m, n - 1);
		}
		SWAP(a[0], a[p]);
		uint64_t pv = a[0].pblk;
		size_t i = 0, j = n;	/* Hoare: a[1..i] < pv, a[j..n-1] > pv */
		for (;;) {
			while (a[++i].pblk < pv && i < n - 1)
				;
			while (a[--j].pblk > pv)
				;
			if (i >= j)
				break;
			SWAP(a[i], a[j]);
		}
		SWAP(a[0], a[j]);
		/* recurse into the smaller side, loop on the larger */
		if (j < n - 1 - j) {
			intro(a, j, depth);
			a += j + 1;
			n -= j + 1;
		} else {
			intro(a + j + 1, n - 1 - j, depth);
			n = j;
		}
	}
	insertion(a, n);
}

static void s_intro(piece_t *a, size_t n)
{
	int depth = 2 * (64 - __builtin_clzll(n | 1));
	intro(a, n, depth);
}

/* ---- radix ---- */

static int sorted(const piece_t *a, size_t n)
{
	for (size_t i = 1; i < n; i++)
		if (a[i].pblk < a[i - 1].pblk)
			return 0;
	return 1;
}

/* passes and digit width for keys below 2^bits: digits of at most 11 bits */
static int plan_digits(uint64_t all, int *width)
{
	int bits = 64 - __builtin_clzll(all | 1);
	int passes = (bits + 10) / 11;
	*width = (bits + passes - 1) / passes;
	return passes;
}

static piece_t *g_tmp;

static void s_radix(piece_t *a, size_t n)
{
	if (sorted(a, n))
		return;
	uint64_t all = 0;
	for (size_t i = 0; i < n; i++)
		all |= a[i].pblk;
	int w, passes = plan_digits(all, &w);
	uint32_t mask = (1u << w) - 1;
	static uint32_t h[6][2048];
	memset(h, 0, sizeof h);
	for (size_t i = 0; i < n; i++)
		for (int p = 0; p < passes; p++)
			h[p][(a[i].pblk >> (p * w)) & mask]++;
	piece_t *src = a, *dst = g_tmp;
	for (int p = 0; p < passes; p++) {
		uint32_t *c = h[p], sum = 0;
		if (c[(src[0].pblk >> (p * w)) & mask] == n)
			continue;	/* every key has the same digit here */
		for (uint32_t d = 0; d <= mask; d++) {
			uint32_t k = c[d];
			c[d] = sum;
			sum += k;
		}
		for (size_t i = 0; i < n; i++)
			dst[c[(src[i].pblk >> (p * w)) & mask]++] = src[i];
		piece_t *t = src;
		src = dst;
		dst = t;
	}
	if (src != a)
		memcpy(a, src, n * sizeof *a);
}

/* ---- pack: radix over (pblk << 24 | index), then gather ---- */

static uint64_t *g_k0, *g_k1;

static void s_pack(piece_t *a, size_t n)
{
	if (sorted(a, n))
		return;
	uint64_t all = 0;
	for (size_t i = 0; i < n; i++)
		all |= a[i].pblk;
	if (n > (1u << 24) || (all >> 40)) {	/* doesn't pack: keys too wide, or too many */
		s_radix(a, n);
		return;
	}
	uint64_t *src = g_k0, *dst = g_k1;
	for (size_t i = 0; i < n; i++)
		src[i] = a[i].pblk << 24 | i;
	int w, passes = plan_digits(all, &w);
	uint32_t mask = (1u << w) - 1;
	static uint32_t h[6][2048];
	memset(h, 0, sizeof h);
	for (size_t i = 0; i < n; i++)
		for (int p = 0; p < passes; p++)
			h[p][(src[i] >> (24 + p * w)) & mask]++;
	for (int p = 0; p < passes; p++) {
		uint32_t *c = h[p], sum = 0;
		int sh = 24 + p * w;
		if (c[(src[0] >> sh) & mask] == n)
			continue;
		for (uint32_t d = 0; d <= mask; d++) {
			uint32_t k = c[d];
			c[d] = sum;
			sum += k;
		}
		for (size_t i = 0; i < n; i++)
			dst[c[(src[i] >> sh) & mask]++] = src[i];
		uint64_t *t = src;
		src = dst;
		dst = t;
	}
	for (size_t i = 0; i < n; i++)
		g_tmp[i] = a[src[i] & 0xFFFFFF];
	memcpy(a, g_tmp, n * sizeof *a);
}

/* ---- harness ---- */

static const struct {
	const char *name;
	void (*fn)(piece_t *, size_t);
} ALG[] = { { "qsort", s_qsort }, { "intro", s_intro }, { "radix", s_radix }, { "pack", s_pack } };
#define NALG (sizeof ALG / sizeof *ALG)

static uint64_t ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
	int reps = 51;
	printf("%-30s %7s", "plan", "n");
	for (size_t k = 0; k < NALG; k++)
		printf(" %9s", ALG[k].name);
	printf("   (median us)\n");
	for (int ai = 1; ai < argc; ai++) {
		if (!strcmp(argv[ai], "-r") && ai + 1 < argc) {
			reps = atoi(argv[++ai]);
			continue;
		}
		FILE *f = fopen(argv[ai], "rb");
		if (!f || fseek(f, 0, SEEK_END))
			return perror(argv[ai]), 1;
		size_t n = (size_t)ftell(f) / sizeof(piece_t);
		rewind(f);
		piece_t *orig = malloc(n * sizeof *orig), *ref = malloc(n * sizeof *ref),
			*a = malloc(n * sizeof *a);
		g_tmp = malloc(n * sizeof *g_tmp);
		g_k0 = malloc(n * 8);
		g_k1 = malloc(n * 8);
		if (fread(orig, sizeof *orig, n, f) != n)
			return perror(argv[ai]), 1;
		fclose(f);
		memcpy(ref, orig, n * sizeof *ref);
		s_qsort(ref, n);
		const char *base = strrchr(argv[ai], '/');
		printf("%-30.30s %7zu", base ? base + 1 : argv[ai], n);
		uint64_t *t = malloc(reps * sizeof *t);
		for (size_t k = 0; k < NALG; k++) {
			for (int r = 0; r < reps; r++) {
				memcpy(a, orig, n * sizeof *a);
				uint64_t t0 = ns();
				ALG[k].fn(a, n);
				t[r] = ns() - t0;
				if (memcmp(a, ref, n * sizeof *a)) {
					fprintf(stderr, "\n%s: wrong result\n", ALG[k].name);
					return 1;
				}
			}
			qsort(t, reps, sizeof *t, cmp_u64);
			printf(" %9.1f", t[reps / 2] / 1e3);
		}
		printf("\n");
		free(t), free(orig), free(ref), free(a), free(g_tmp), free(g_k0), free(g_k1);
	}
	return 0;
}
