/*
 * pool: see pool.h. A job is published under the mutex (so everything the
 * caller wrote before pool_for() is visible to the workers), indices are
 * handed out lock-free, and each worker checks out under the mutex again (so
 * everything the workers wrote is visible to the caller when pool_for()
 * returns). The only atomic is the index counter, and it can be relaxed.
 */
#include "pool.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

struct pool {
	pthread_mutex_t mu;
	pthread_cond_t go, done;
	pthread_t *thr;
	unsigned workers;	/* threads other than the caller */
	/* the current job; all but next are written only under mu */
	pool_fn fn;
	void *ctx;
	size_t n;
	unsigned gen, busy;
	int quit;
	_Atomic size_t next;
};

static void claim(pool *p, pool_fn fn, void *ctx, size_t n)
{
	for (size_t i; (i = atomic_fetch_add_explicit(&p->next, 1, memory_order_relaxed)) < n;)
		fn(ctx, i);
}

static void *worker(void *arg)
{
	pool *p = arg;
	unsigned seen = 0;
	pthread_mutex_lock(&p->mu);
	for (;;) {
		while (p->gen == seen && !p->quit)
			pthread_cond_wait(&p->go, &p->mu);
		if (p->quit)
			break;
		seen = p->gen;
		pool_fn fn = p->fn;
		void *ctx = p->ctx;
		size_t n = p->n;
		pthread_mutex_unlock(&p->mu);
		claim(p, fn, ctx, n);
		pthread_mutex_lock(&p->mu);
		if (--p->busy == 0)
			pthread_cond_signal(&p->done);
	}
	pthread_mutex_unlock(&p->mu);
	return NULL;
}

pool *pool_new(unsigned threads)
{
	pool *p = calloc(1, sizeof *p);
	if (!p)
		return NULL;
	pthread_mutex_init(&p->mu, NULL);
	pthread_cond_init(&p->go, NULL);
	pthread_cond_init(&p->done, NULL);
	p->workers = threads > 1 ? threads - 1 : 0;
	p->thr = calloc(p->workers ? p->workers : 1, sizeof *p->thr);
	for (unsigned i = 0; i < p->workers; i++)
		if (!p->thr || pthread_create(&p->thr[i], NULL, worker, p)) {
			p->workers = i;	/* run with what we have */
			break;
		}
	return p;
}

void pool_for(pool *p, size_t n, pool_fn fn, void *ctx)
{
	if (!p->workers) {
		for (size_t i = 0; i < n; i++)
			fn(ctx, i);
		return;
	}
	pthread_mutex_lock(&p->mu);
	p->fn = fn;
	p->ctx = ctx;
	p->n = n;
	atomic_store_explicit(&p->next, 0, memory_order_relaxed);
	p->busy = p->workers;
	p->gen++;
	pthread_cond_broadcast(&p->go);
	pthread_mutex_unlock(&p->mu);
	claim(p, fn, ctx, n);
	pthread_mutex_lock(&p->mu);
	while (p->busy)
		pthread_cond_wait(&p->done, &p->mu);
	pthread_mutex_unlock(&p->mu);
}

void pool_free(pool *p)
{
	pthread_mutex_lock(&p->mu);
	p->quit = 1;
	pthread_cond_broadcast(&p->go);
	pthread_mutex_unlock(&p->mu);
	for (unsigned i = 0; i < p->workers; i++)
		pthread_join(p->thr[i], NULL);
	pthread_cond_destroy(&p->go);
	pthread_cond_destroy(&p->done);
	pthread_mutex_destroy(&p->mu);
	free(p->thr);
	free(p);
}
