/*
 * b3tree: our BLAKE3 tree layer over the upstream compression kernels.
 *
 * Upstream's hasher walks a subtree depth-first to keep its stack small. We
 * never hash more than 1 MiB (1024 chunks) in one go, so we can afford to go
 * breadth-first instead: every full chunk in one hash_many() call, then each
 * layer of parents in one more, ping-ponging between two 32 KiB CV arrays.
 * Pairing neighbours and carrying an odd one up a layer builds exactly
 * BLAKE3's left-complete tree, so no left_subtree_len() is needed below 1 MiB.
 */
#include "b3tree.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/*
 * The upstream kernel ABI (blake3_impl.h of BLAKE3 1.8.7, vendored). Either
 * fixed at compile time (-march=native knows the CPU), or, with B3_DISPATCH,
 * picked once at start-up from what the CPU has, as the Rust crate does, so
 * one binary runs well on any x86-64.
 */
#define HASH_MANY(k)                                                                        \
	void blake3_hash_many_##k(const uint8_t *const *inputs, size_t num_inputs,          \
				  size_t blocks, const uint32_t key[8], uint64_t counter,   \
				  bool increment_counter, uint8_t flags,                    \
				  uint8_t flags_start, uint8_t flags_end, uint8_t *out)
#define COMPRESS(k)                                                                         \
	void blake3_compress_in_place_##k(uint32_t cv[8], const uint8_t block[64],          \
					  uint8_t block_len, uint64_t counter, uint8_t flags)

#if defined(B3_DISPATCH)
HASH_MANY(avx512);
HASH_MANY(avx2);
HASH_MANY(sse41);
HASH_MANY(sse2);
COMPRESS(avx512);
COMPRESS(sse41);
COMPRESS(sse2);
static void (*hash_many)(const uint8_t *const *, size_t, size_t, const uint32_t[8], uint64_t, bool,
			 uint8_t, uint8_t, uint8_t, uint8_t *);
static void (*compress)(uint32_t cv[8], const uint8_t block[64], uint8_t block_len,
			uint64_t counter, uint8_t flags);
static const char *kname;

const char *b3_kernel(void)
{
	return kname;
}

__attribute__((constructor)) static void pick(void)
{
	/* B3_FORCE=sse41|sse2 steps down, so a test can reach the narrower kernels */
	const char *f = getenv("B3_FORCE");
	int down = f ? (!strcmp(f, "sse2") ? 2 : !strcmp(f, "sse41") ? 1 : 0) : 0;
	__builtin_cpu_init();
	if (down == 2) {
		hash_many = blake3_hash_many_sse2, compress = blake3_compress_in_place_sse2;
		kname = "sse2 forced";
	} else if (down == 1) {
		hash_many = blake3_hash_many_sse41, compress = blake3_compress_in_place_sse41;
		kname = "sse41 forced";
	} else if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512vl")) {
		hash_many = blake3_hash_many_avx512, compress = blake3_compress_in_place_avx512;
		kname = "avx512 dispatched";
	} else if (__builtin_cpu_supports("avx2")) {
		hash_many = blake3_hash_many_avx2, compress = blake3_compress_in_place_sse41;
		kname = "avx2+sse41 dispatched";
	} else if (__builtin_cpu_supports("sse4.1")) {
		hash_many = blake3_hash_many_sse41, compress = blake3_compress_in_place_sse41;
		kname = "sse41 dispatched";
	} else {	/* every x86-64 has SSE2 */
		hash_many = blake3_hash_many_sse2, compress = blake3_compress_in_place_sse2;
		kname = "sse2 dispatched";
	}
}
#elif defined(B3_PORTABLE) || !defined(__AVX2__)
#define hash_many blake3_hash_many_portable
#define compress blake3_compress_in_place_portable
HASH_MANY(portable);
COMPRESS(portable);
const char *b3_kernel(void)
{
	return "portable";
}
#else
#define hash_many blake3_hash_many_avx2
#define compress blake3_compress_in_place_sse41
HASH_MANY(avx2);
COMPRESS(sse41);
#ifndef B3_FLAVOUR
#define B3_FLAVOUR ""
#endif
const char *b3_kernel(void)
{
	return "avx2+sse41" B3_FLAVOUR;
}
#endif

enum { CHUNK_START = 1, CHUNK_END = 2, PARENT = 4, ROOT = 8 };
/*
 * Chunks hashed breadth-first at once, a power of two >= 2. 1024 (1 MiB) keeps a
 * SIMD kernel fed with 8 or 16 inputs per call all the way up, at the price
 * of ~56 KiB of stack. The portable kernel hashes one input at a time, so
 * width buys it nothing, and an Amiga task's stack is a few KiB: 16 chunks
 * needs under 1 KiB. Wider pieces are split at b3_left_len(), so any width
 * builds the same tree.
 */
#ifndef B3_WIDE
#if defined(B3_PORTABLE) || (!defined(B3_DISPATCH) && !defined(__AVX2__))
#define B3_WIDE 16
#else
#define B3_WIDE 1024
#endif
#endif
#define WIDE ((size_t)B3_WIDE)

static const uint32_t IV[8] = { 0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
				0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19 };

static void store_cv(uint8_t out[B3_OUT], const uint32_t w[8])
{
	for (int i = 0; i < 8; i++) {	/* little-endian on any host, e.g. m68k */
		out[4 * i] = (uint8_t)w[i];
		out[4 * i + 1] = (uint8_t)(w[i] >> 8);
		out[4 * i + 2] = (uint8_t)(w[i] >> 16);
		out[4 * i + 3] = (uint8_t)(w[i] >> 24);
	}
}

/* One chunk of at most 1 KiB, block by block; root adds ROOT to its last block. */
static void chunk(const uint8_t *in, size_t len, uint64_t ctr, uint8_t root, uint8_t out[B3_OUT])
{
	uint32_t cv[8];
	uint8_t blk[64] = { 0 }, start = CHUNK_START;
	memcpy(cv, IV, sizeof cv);
	for (; len > 64; in += 64, len -= 64, start = 0)
		compress(cv, in, 64, ctr, start);
	memcpy(blk, in, len);
	compress(cv, blk, (uint8_t)len, ctr, start | CHUNK_END | root);
	store_cv(out, cv);
}

static void node(const uint8_t *l, uint8_t flags, uint8_t out[B3_OUT])
{
	uint32_t cv[8];
	memcpy(cv, IV, sizeof cv);
	compress(cv, l, 64, 0, flags);	/* l and r are adjacent: one 64-byte block */
	store_cv(out, cv);
}

void b3_parent(const uint8_t l[B3_OUT], const uint8_t r[B3_OUT], uint8_t cv[B3_OUT])
{
	uint8_t blk[64];
	memcpy(blk, l, B3_OUT);
	memcpy(blk + B3_OUT, r, B3_OUT);
	node(blk, PARENT, cv);
}

void b3_root_parent(const uint8_t l[B3_OUT], const uint8_t r[B3_OUT], uint8_t out[B3_OUT])
{
	uint8_t blk[64];
	memcpy(blk, l, B3_OUT);
	memcpy(blk + B3_OUT, r, B3_OUT);
	node(blk, PARENT | ROOT, out);
}

/*
 * GCC reads hash_many()'s pointer-array argument as "may be uninitialised"
 * because only its first n entries are set; the kernel reads only those.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

/*
 * 1 KiB < len <= 1 MiB: hash breadth-first down to `keep` CVs (1 for a
 * subtree, 2 for a root, which the caller then finalises). Returns the array
 * holding them.
 */
static const uint8_t *wide(const uint8_t *in, size_t len, uint64_t ctr, size_t keep,
			   uint8_t (*a)[B3_OUT], uint8_t (*b)[B3_OUT])
{
	const uint8_t *p[WIDE];
	size_t n = len / B3_CHUNK;
	for (size_t i = 0; i < n; i++)
		p[i] = in + i * B3_CHUNK;
	hash_many(p, n, B3_CHUNK / 64, IV, ctr, true, 0, CHUNK_START, CHUNK_END, a[0]);
	if (len % B3_CHUNK) {
		chunk(in + n * B3_CHUNK, len % B3_CHUNK, ctr + n, 0, a[n]);
		n++;
	}
	while (n > keep) {
		size_t np = n / 2;
		for (size_t i = 0; i < np; i++)
			p[i] = a[2 * i];
		hash_many(p, np, 1, IV, 0, false, PARENT, 0, 0, b[0]);
		if (n & 1)
			memcpy(b[np], a[n - 1], B3_OUT);	/* odd one out goes up a layer */
		n = np + (n & 1);
		uint8_t (*t)[B3_OUT] = a;
		a = b;
		b = t;
	}
	return a[0];
}

void b3_subtree(const uint8_t *in, size_t len, uint64_t off, uint8_t cv[B3_OUT])
{
	uint64_t ctr = off / B3_CHUNK;
	if (len <= B3_CHUNK) {
		chunk(in, len, ctr, 0, cv);
	} else if (len <= WIDE * B3_CHUNK) {
		uint8_t a[WIDE][B3_OUT], b[WIDE / 2][B3_OUT];
		memcpy(cv, wide(in, len, ctr, 1, a, b), B3_OUT);
	} else {
		uint8_t l[B3_OUT], r[B3_OUT];
		size_t ll = b3_left_len(len);
		b3_subtree(in, ll, off, l);
		b3_subtree(in + ll, len - ll, off + ll, r);
		b3_parent(l, r, cv);
	}
}

void b3_hash(const uint8_t *in, size_t len, uint8_t out[B3_OUT])
{
	if (len <= B3_CHUNK) {
		chunk(in, len, 0, ROOT, out);
	} else if (len <= WIDE * B3_CHUNK) {
		uint8_t a[WIDE][B3_OUT], b[WIDE / 2][B3_OUT];
		node(wide(in, len, 0, 2, a, b), PARENT | ROOT, out);
	} else {
		uint8_t l[B3_OUT], r[B3_OUT];
		size_t ll = b3_left_len(len);
		b3_subtree(in, ll, 0, l);
		b3_subtree(in + ll, len - ll, ll, r);
		b3_root_parent(l, r, out);
	}
}

static void merge(const uint8_t (*cv)[B3_OUT], size_t n, uint8_t out[B3_OUT])
{
	if (n == 1) {
		memcpy(out, cv[0], B3_OUT);
		return;
	}
	uint8_t l[B3_OUT], r[B3_OUT];
	size_t k = (size_t)1 << (63 - __builtin_clzll(n - 1));	/* largest power of two < n */
	merge(cv, k, l);
	merge(cv + k, n - k, r);
	b3_parent(l, r, out);
}

void b3_root_of_pieces(const uint8_t (*cv)[B3_OUT], size_t n, uint8_t out[B3_OUT])
{
	uint8_t l[B3_OUT], r[B3_OUT];
	size_t k = (size_t)1 << (63 - __builtin_clzll(n - 1));
	merge(cv, k, l);
	merge(cv + k, n - k, r);
	b3_root_parent(l, r, out);
}
