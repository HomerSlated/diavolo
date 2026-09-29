/*
 * b3tree: BLAKE3 subtree chaining values for pieces of a file that arrive in
 * any order -- the C counterpart of Rust's blake3::hazmat.
 *
 * A piece is [off, off+len) of the file's logical bytes where off is a
 * multiple of the smallest power of two >= len (and >= 1 KiB), i.e. the piece
 * is a whole subtree of the BLAKE3 tree; only a file's final piece may be
 * shorter than its alignment. Such a piece's chaining value (CV) depends only
 * on its bytes and its offset, so pieces can be hashed as they are read, in
 * disk order, and merged afterwards with b3_parent()/b3_root_parent().
 *
 * The one exception is a piece that is the entire file: it has no parent, so
 * it must be finalised as the root with b3_hash() instead.
 *
 * Kernels (SIMD compression) are the upstream BLAKE3 ones, chosen at compile
 * time; see the Makefile. The tree logic here is our own.
 */
#ifndef B3TREE_H
#define B3TREE_H

#include <stddef.h>
#include <stdint.h>

#define B3_CHUNK 1024u
#define B3_OUT 32u

/* Largest power of two strictly less than len, in whole chunks; len > 1 KiB. */
static inline uint64_t b3_left_len(uint64_t len)
{
	uint64_t full = (len - 1) / B3_CHUNK;	/* chunks, less a possibly-short last one */
	return (uint64_t)B3_CHUNK << (63 - __builtin_clzll(full));
}

/* CV of the subtree of len bytes starting at file offset off (non-root). */
void b3_subtree(const uint8_t *in, size_t len, uint64_t off, uint8_t cv[B3_OUT]);

/* Parent CV of two sibling subtrees (non-root), and the root of the file. */
void b3_parent(const uint8_t l[B3_OUT], const uint8_t r[B3_OUT], uint8_t cv[B3_OUT]);
void b3_root_parent(const uint8_t l[B3_OUT], const uint8_t r[B3_OUT], uint8_t out[B3_OUT]);

/*
 * Root of a file cut into n >= 2 pieces of 1 MiB (the last may be shorter),
 * given their CVs in file order. Pieces may be hashed in any order; only this
 * final pass needs them in order, as 32 bytes per MiB.
 */
void b3_root_of_pieces(const uint8_t (*cv)[B3_OUT], size_t n, uint8_t out[B3_OUT]);

/* Plain BLAKE3 of a whole input (the root); equals b3sum. */
void b3_hash(const uint8_t *in, size_t len, uint8_t out[B3_OUT]);

/* The name of the compiled-in kernel set. */
const char *b3_kernel(void);

#endif
