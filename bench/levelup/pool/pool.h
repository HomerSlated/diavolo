/*
 * pool: a persistent pthreads worker pool with one operation, a parallel
 * for-loop over [0, n). Workers claim indices with an atomic fetch-add, one
 * at a time, so uneven items balance themselves; the calling thread works
 * too. This is the part of rayon that Diavolo's hashing needs: pieces are
 * independent, so there is no nesting and nothing to steal.
 */
#ifndef POOL_H
#define POOL_H

#include <stddef.h>

typedef struct pool pool;
typedef void (*pool_fn)(void *ctx, size_t i);

/* threads counts the caller: pool_new(1) runs everything inline. */
pool *pool_new(unsigned threads);
void pool_for(pool *p, size_t n, pool_fn fn, void *ctx);
void pool_free(pool *p);

#endif
