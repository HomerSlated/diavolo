/*
 * radix.h: LSD radix sort on a 64-bit key, generated per element type.
 *
 *   RADIX_DEFINE(name, T, KEY)   defines  static void name(T *a, size_t n, T *tmp)
 *
 * KEY(x) yields the uint64_t key of the element x (an lvalue of type T); tmp
 * must hold n elements, and n must fit in 32 bits. The sort is stable. It:
 *   - returns at once if already sorted (one O(n) scan);
 *   - insertion sort below 64 elements, where histograms cost more than they save;
 *   - pass count and digit width from the OR of all keys: digits of at most 11
 *     bits (2048 buckets: 8-bit digits need a third pass on 18-21 bit block
 *     numbers and measured ~2x slower);
 *   - every pass's histogram in one scan; passes where all keys share a digit
 *     are skipped.
 * Measured against glibc qsort and Rust's sorts in sortbench.c / ../README.md.
 */
#ifndef RADIX_H
#define RADIX_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RADIX_DEFINE(name, T, KEY)                                                         \
	static void name(T *a, size_t n, T *tmp)                                             \
	{                                                                                    \
		size_t i_;                                                                   \
		for (i_ = 1; i_ < n && KEY(a[i_ - 1]) <= KEY(a[i_]); i_++)                    \
			;                                                                    \
		if (i_ >= n)                                                                 \
			return;                                                              \
		if (n < 64) {                                                                \
			for (; i_ < n; i_++) {                                               \
				T x_ = a[i_];                                                \
				size_t j_ = i_;                                              \
				for (; j_ > 0 && KEY(a[j_ - 1]) > KEY(x_); j_--)            \
					a[j_] = a[j_ - 1];                                   \
				a[j_] = x_;                                                  \
			}                                                                    \
			return;                                                              \
		}                                                                            \
		uint64_t all_ = 0;                                                           \
		for (size_t k_ = 0; k_ < n; k_++)                                            \
			all_ |= KEY(a[k_]);                                                  \
		int bits_ = 64 - __builtin_clzll(all_ | 1);                                  \
		int passes_ = (bits_ + 10) / 11, w_ = (bits_ + passes_ - 1) / passes_;       \
		uint64_t mask_ = ((uint64_t)1 << w_) - 1;                                    \
		uint32_t h_[6][2048];                                                        \
		memset(h_, 0, sizeof h_[0] * (size_t)passes_);                               \
		for (size_t k_ = 0; k_ < n; k_++)                                            \
			for (int p_ = 0; p_ < passes_; p_++)                                 \
				h_[p_][(KEY(a[k_]) >> (p_ * w_)) & mask_]++;                 \
		T *src_ = a, *dst_ = tmp;                                                    \
		for (int p_ = 0; p_ < passes_; p_++) {                                       \
			uint32_t *c_ = h_[p_], sum_ = 0;                                     \
			if (c_[(KEY(src_[0]) >> (p_ * w_)) & mask_] == n)                    \
				continue; /* every key has the same digit here */            \
			for (uint64_t d_ = 0; d_ <= mask_; d_++) {                           \
				uint32_t k_ = c_[d_];                                        \
				c_[d_] = sum_;                                               \
				sum_ += k_;                                                  \
			}                                                                    \
			for (size_t k_ = 0; k_ < n; k_++)                                    \
				dst_[c_[(KEY(src_[k_]) >> (p_ * w_)) & mask_]++] = src_[k_]; \
			T *t_ = src_;                                                        \
			src_ = dst_;                                                         \
			dst_ = t_;                                                           \
		}                                                                            \
		if (src_ != a)                                                               \
			memcpy(a, src_, n * sizeof *a);                                      \
	}

#endif
