/*
 * accbench — storage access-method benchmark, C implementation.
 *
 * Reads every regular file on one filesystem by one access method and reports
 * how long it spent finding the data versus reading it. The Rust program in
 * ../rust is a line-for-line peer: same methods, same syscalls, same buffer
 * size, same hash, same output; the Rust side uses its own BLAKE3, sort and
 * thread pool (blake3::hazmat, and ports of the radix sort and the pool).
 * See ../README.md.
 *
 *   accbench vfs      MOUNTPOINT   method 1: lstat + open + read by path
 *   accbench handle   MOUNTPOINT   method 2: walk, name_to_handle_at, inode order, open_by_handle_at
 *   accbench bulkstat MOUNTPOINT   method 2: XFS_IOC_BULKSTAT + open_by_handle_at (xfsdump's way)
 *   accbench e2fs     DEVICE       method 3: libext2fs inode scan, own pread of data (dump's way)
 *   accbench raw      DEVICE       method 3: hand-written ext4 parser, own pread of data
 *   accbench rawsort  DEVICE       method 3: as raw, but all extents of all files in disk order
 *
 * Options: --hash (BLAKE3 of all file content, holes as zeros: equals b3sum),
 * --digest FILE (implies --hash; write "ino size digest" per file, sorted by
 * inode), --threads N (hash on N threads; default 1, inline), --header (print
 * CSV header only).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ext2fs/ext2fs.h>
#include <xfs/xfs.h>

#include "../../levelup/b3tree/b3tree.h"
#include "../../levelup/pool/pool.h"
#include "../../levelup/sort/radix.h"

#ifdef __clang__
static const char *IMPL = "c-clang";
#else
static const char *IMPL = "c-gcc";
#endif

#define BUFSZ (1u << 20)	/* every content read is at most 1 MiB */
#define HK 0x9E3779B97F4A7C15ull

static uint8_t *g_buf;
static int g_hash;
static uint64_t g_files, g_names, g_bytes, g_read_ns, g_combined;

__attribute__((noreturn, format(printf, 1, 2)))
static void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "accbench: ");
	vfprintf(stderr, fmt, ap);
	if (errno)
		fprintf(stderr, ": %s", strerror(errno));
	fputc('\n', stderr);
	va_end(ap);
	exit(2);
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static inline uint64_t fmix(uint64_t h)
{
	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdull;
	h ^= h >> 33;
	h *= 0xc4ceb9fe1a85ec53ull;
	h ^= h >> 33;
	return h;
}

/* ---- per-file records ---- */

typedef struct {
	uint64_t ino, size;
	uint8_t digest[B3_OUT];
} rec_t;

static rec_t *g_recs;
static size_t g_nrecs, g_caprecs;

static void add_record(uint64_t ino, uint64_t size, const uint8_t *digest)
{
	if (g_nrecs == g_caprecs) {
		g_caprecs = g_caprecs ? g_caprecs * 2 : 4096;
		g_recs = realloc(g_recs, g_caprecs * sizeof *g_recs);
		if (!g_recs)
			die("out of memory");
	}
	rec_t *r = &g_recs[g_nrecs++];
	r->ino = ino;
	r->size = size;
	g_files++;
	g_bytes += size;
	if (digest) {
		uint64_t w;
		memcpy(r->digest, digest, B3_OUT);
		memcpy(&w, digest, 8);	/* little-endian host assumed (x86-64) */
		g_combined += fmix(w ^ (ino * HK));
	} else {
		memset(r->digest, 0, B3_OUT);
	}
}

#define INO(x) ((x).ino)
RADIX_DEFINE(sort_recs, rec_t, INO)

/* ---- inode set: open addressing, key 0 = empty (inode 0 never exists) ---- */

typedef struct {
	uint64_t *k;
	size_t cap, n;
} iset;

static int iset_add(iset *s, uint64_t ino)
{
	if ((s->n + 1) * 2 > s->cap) {
		size_t oc = s->cap, nc = oc ? oc * 2 : 1024;
		uint64_t *ok = s->k, *nk = calloc(nc, sizeof *nk);
		if (!nk)
			die("out of memory");
		for (size_t i = 0; i < oc; i++) {
			if (!ok[i])
				continue;
			size_t j = fmix(ok[i]) & (nc - 1);
			while (nk[j])
				j = (j + 1) & (nc - 1);
			nk[j] = ok[i];
		}
		free(ok);
		s->k = nk;
		s->cap = nc;
	}
	size_t m = s->cap - 1, i = fmix(ino) & m;
	while (s->k[i]) {
		if (s->k[i] == ino)
			return 0;
		i = (i + 1) & m;
	}
	s->k[i] = ino;
	s->n++;
	return 1;
}

/*
 * ---- content hash: BLAKE3 of each file's logical bytes, holes as zeros ----
 *
 * Every method delivers a file's bytes as (logical offset, data), in whatever
 * order it reads them. Each delivery is split into aligned power-of-two
 * subtrees of 1 KiB..1 MiB, hashed to BLAKE3 chaining values on the spot
 * (b3tree), and kept as 48 bytes each. When a file's delivered bytes reach
 * the bytes it has on disk, its subtrees are merged in tree order, with holes
 * and uninitialised extents hashed as zero subtrees. The digest equals b3sum
 * of the file, whatever the arrival order, and no data is ever held back.
 *
 * With --threads N > 1, reads land in an arena of 2N MiB instead of g_buf;
 * when it fills, the pool hashes all its subtrees at once, then the files
 * they completed are merged. The Rust peer does the same with a port of the
 * pool.
 */

typedef struct {
	uint64_t off, len;
	uint8_t cv[B3_OUT];
} run_t;

typedef struct {
	uint64_t ino, size, need, got;	/* need: bytes to be delivered (size less holes) */
	run_t *r;
	uint32_t nr, cap;
	int whole;	/* r[0] is the entire file, so its cv is the root */
	int done;
} hfile_t;

static hfile_t *g_hf;
static size_t g_nhf, g_caphf;
static uint8_t g_zero[1u << 20];	/* never written: .bss, not file size */
static run_t *g_rtmp;
static size_t g_caprtmp;

/* batch mode (--threads > 1) */
typedef struct {
	const uint8_t *p;
	uint32_t f, i;	/* file, run index: the run arrays may move until the flush */
} job_t;

static unsigned g_threads = 1;
static pool *g_pool;
static uint8_t *g_arena;
static size_t g_arena_sz, g_fill;
static job_t *g_jobs;
static size_t g_njobs, g_capjobs;
static uint32_t *g_done;
static size_t g_ndone, g_capdone;

#define GROW(v, n, cap, init)                                                          \
	do {                                                                           \
		if ((n) == (cap)) {                                                    \
			(cap) = (cap) ? (cap) * 2 : (init);                            \
			if (!((v) = realloc((v), (cap) * sizeof *(v))))                \
				die("out of memory");                                  \
		}                                                                      \
	} while (0)

static void zero_cv(uint64_t off, uint64_t len, uint8_t cv[B3_OUT])
{
	if (len > sizeof g_zero) {	/* a long hole: its own subtrees, merged */
		uint8_t l[B3_OUT], r[B3_OUT];
		uint64_t ll = b3_left_len(len);
		zero_cv(off, ll, l);
		zero_cv(off + ll, len - ll, r);
		b3_parent(l, r, cv);
	} else {
		b3_subtree(g_zero, len, off, cv);
	}
}

/* first run with offset >= off; runs are sorted by offset */
static uint32_t lower(const hfile_t *f, uint64_t off)
{
	uint32_t lo = 0, hi = f->nr;
	while (lo < hi) {
		uint32_t m = (lo + hi) / 2;
		if (f->r[m].off < off)
			lo = m + 1;
		else
			hi = m;
	}
	return lo;
}

static void node(const hfile_t *f, uint64_t off, uint64_t len, uint8_t cv[B3_OUT])
{
	uint32_t i = lower(f, off), j;
	if (i < f->nr && f->r[i].off == off && f->r[i].len == len) {
		memcpy(cv, f->r[i].cv, B3_OUT);
		return;
	}
	j = lower(f, off + len);	/* any run inside [off, off+len)? */
	if (!(j > 0 && f->r[j - 1].off + f->r[j - 1].len > off)) {
		zero_cv(off, len, cv);	/* a hole */
		return;
	}
	if (len <= B3_CHUNK) {
		errno = 0;
		die("ino %" PRIu64 ": subtree misaligned at %" PRIu64, f->ino, off);
	}
	uint8_t l[B3_OUT], r[B3_OUT];
	uint64_t ll = b3_left_len(len);
	node(f, off, ll, l);
	node(f, off + ll, len - ll, r);
	b3_parent(l, r, cv);
}

#define OFF(x) ((x).off)
RADIX_DEFINE(sort_runs, run_t, OFF)

static void hf_finish(uint32_t fid)
{
	hfile_t *f = &g_hf[fid];
	uint8_t d[B3_OUT];
	if (f->whole) {
		memcpy(d, f->r[0].cv, B3_OUT);
	} else if (f->size <= B3_CHUNK) {
		b3_hash(g_zero, f->size, d);	/* empty, or all hole */
	} else {
		if (f->nr > g_caprtmp && !(g_rtmp = realloc(g_rtmp, (g_caprtmp = f->nr) * sizeof *g_rtmp)))
			die("out of memory");
		sort_runs(f->r, f->nr, g_rtmp);
		uint8_t l[B3_OUT], r[B3_OUT];
		uint64_t ll = b3_left_len(f->size);
		node(f, 0, ll, l);
		node(f, ll, f->size - ll, r);
		b3_root_parent(l, r, d);
	}
	add_record(f->ino, f->size, d);
	f->done = 1;
	free(f->r);
	f->r = NULL;
	f->nr = f->cap = 0;
}

/* Start hashing a file of size bytes, need of them to be delivered. */
static uint32_t hf_open(uint64_t ino, uint64_t size, uint64_t need)
{
	GROW(g_hf, g_nhf, g_caphf, 4096);
	uint32_t fid = (uint32_t)g_nhf++;
	g_hf[fid] = (hfile_t){ .ino = ino, .size = size, .need = need };
	if (!need)
		hf_finish(fid);	/* empty or all hole: nothing will arrive */
	return fid;
}

static void job_run(void *ctx, size_t i)
{
	(void)ctx;
	const job_t *j = &g_jobs[i];
	const hfile_t *f = &g_hf[j->f];
	run_t *r = &f->r[j->i];
	if (f->whole)
		b3_hash(j->p, r->len, r->cv);
	else
		b3_subtree(j->p, r->len, r->off, r->cv);
}

static void hb_flush(void)
{
	if (g_njobs)
		pool_for(g_pool, g_njobs, job_run, NULL);
	g_njobs = g_fill = 0;
	for (size_t i = 0; i < g_ndone; i++)
		hf_finish(g_done[i]);
	g_ndone = 0;
}

/*
 * Where to read the next want (<= BUFSZ) bytes that will be delivered; then
 * hb_used(bytes read), once the deliveries from that read are made.
 */
static uint8_t *hb_buf(size_t want)
{
	if (g_threads == 1 || !g_hash)
		return g_buf;	/* delivered and hashed before the next read */
	if (g_fill + want > g_arena_sz)
		hb_flush();
	return g_arena + g_fill;
}

static void hb_used(size_t n)
{
	if (g_threads > 1 && g_hash)
		g_fill += (n + 63) & ~(size_t)63;
}

static void deliver(uint32_t fid, uint64_t off, const uint8_t *p, size_t n)
{
	hfile_t *f = &g_hf[fid];
	if (off % B3_CHUNK || (n % B3_CHUNK && off + n != f->size) || off + n > f->size) {
		errno = 0;
		die("ino %" PRIu64 ": delivery %" PRIu64 "+%zu misaligned or past EOF", f->ino, off, n);
	}
	int batch = g_threads > 1;
	f->got += n;
	f->whole = off == 0 && n == f->size;
	for (uint64_t a = off, b = off + n; a < b;) {
		uint64_t sz = sizeof g_zero;
		while (sz > B3_CHUNK && (a % sz || a + sz > b))
			sz >>= 1;
		uint64_t len = sz < b - a ? sz : b - a;	/* the file's last chunk may be short */
		GROW(f->r, f->nr, f->cap, 4);
		run_t *r = &f->r[f->nr];
		r->off = a;
		r->len = f->whole ? n : len;
		if (batch) {
			GROW(g_jobs, g_njobs, g_capjobs, 1024);
			g_jobs[g_njobs++] = (job_t){ p + (a - off), (uint32_t)fid, f->nr };
		} else if (f->whole) {
			b3_hash(p, n, r->cv);
		} else {
			b3_subtree(p + (a - off), len, a, r->cv);
		}
		f->nr++;
		if (f->whole)
			break;
		a += len;
	}
	if (f->got == f->need) {
		if (batch) {
			GROW(g_done, g_ndone, g_capdone, 1024);
			g_done[g_ndone++] = fid;
		} else {
			hf_finish(fid);
		}
	}
}

/* ---- the shared read loop for fd-based methods (1 and 2) ---- */

static uint64_t read_stream(int fd, uint64_t ino, uint64_t size)
{
	uint64_t tot = 0;
	uint32_t fid = g_hash ? hf_open(ino, size, size) : 0;
	for (;;) {
		uint64_t t0 = now_ns();
		uint8_t *buf = hb_buf(BUFSZ);
		size_t got = 0;
		ssize_t r = 1;
		while (got < BUFSZ && (r = read(fd, buf + got, BUFSZ - got)) != 0) {
			if (r < 0) {
				if (errno == EINTR)
					continue;
				die("read");
			}
			got += (size_t)r;	/* fill the buffer, so every delivery is aligned */
		}
		if (g_hash && got) {
			if (tot + got > size) {
				errno = 0;
				die("ino %" PRIu64 ": grew while being read", ino);
			}
			deliver(fid, tot, buf, got);
			hb_used(got);
		}
		g_read_ns += now_ns() - t0;
		tot += got;
		if (r == 0)
			break;
	}
	return tot;
}

static inline int dot_or_dotdot(const char *n)
{
	return n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2]));
}

/* ---- method 1: VFS ---- */

static iset g_seen;

static void vfs_dir(int dfd)
{
	DIR *d = fdopendir(dfd);
	struct dirent *e;
	if (!d)
		die("fdopendir");
	for (errno = 0; (e = readdir(d)); errno = 0) {
		const char *nm = e->d_name;
		struct stat st;
		if (dot_or_dotdot(nm))
			continue;
		g_names++;
		if (e->d_type != DT_DIR && e->d_type != DT_REG && e->d_type != DT_UNKNOWN)
			continue;
		/* lstat first, as tar and rsync do: metadata, and hardlink detection before opening */
		if (fstatat(dirfd(d), nm, &st, AT_SYMLINK_NOFOLLOW))
			die("fstatat %s", nm);
		if (S_ISDIR(st.st_mode)) {
			int c = openat(dirfd(d), nm, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (c < 0)
				die("openat dir %s", nm);
			vfs_dir(c);
		} else if (S_ISREG(st.st_mode) && iset_add(&g_seen, st.st_ino)) {
			uint64_t n;
			int f = openat(dirfd(d), nm, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
			if (f < 0)
				die("openat %s", nm);
			n = read_stream(f, st.st_ino, (uint64_t)st.st_size);
			if (n != (uint64_t)st.st_size)
				die("%s: read %" PRIu64 " of %" PRIu64 " bytes", nm, n, (uint64_t)st.st_size);
			if (!g_hash)
				add_record(st.st_ino, n, NULL);
			close(f);
		}
	}
	if (errno)
		die("readdir");
	closedir(d);
}

static void m_vfs(const char *mnt)
{
	int fd = open(mnt, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		die("open %s", mnt);
	vfs_dir(fd);
}

/* ---- method 2a: file handles, any filesystem ---- */

typedef struct {
	uint64_t ino;
	size_t off;	/* into g_harena: a struct file_handle followed by its bytes */
} hrec;

static hrec *g_h;
static size_t g_nh, g_caph;
static uint8_t *g_harena;
static size_t g_harena_n, g_harena_cap;

static void h_push(uint64_t ino, const struct file_handle *fh)
{
	size_t need = sizeof *fh + fh->handle_bytes;
	need = (need + 7) & ~(size_t)7;
	if (g_harena_n + need > g_harena_cap) {
		g_harena_cap = g_harena_cap ? g_harena_cap * 2 : 1 << 20;
		g_harena = realloc(g_harena, g_harena_cap);
		if (!g_harena)
			die("out of memory");
	}
	if (g_nh == g_caph) {
		g_caph = g_caph ? g_caph * 2 : 4096;
		g_h = realloc(g_h, g_caph * sizeof *g_h);
		if (!g_h)
			die("out of memory");
	}
	memcpy(g_harena + g_harena_n, fh, sizeof *fh + fh->handle_bytes);
	g_h[g_nh++] = (hrec){ ino, g_harena_n };
	g_harena_n += need;
}

RADIX_DEFINE(sort_hrecs, hrec, INO)

static void handle_dir(int dfd)
{
	union {
		struct file_handle fh;
		uint8_t raw[sizeof(struct file_handle) + MAX_HANDLE_SZ];
	} u;
	DIR *d = fdopendir(dfd);
	struct dirent *e;
	if (!d)
		die("fdopendir");
	for (errno = 0; (e = readdir(d)); errno = 0) {
		const char *nm = e->d_name;
		unsigned char t = e->d_type;
		if (dot_or_dotdot(nm))
			continue;
		g_names++;
		if (t == DT_UNKNOWN) {
			struct stat st;
			if (fstatat(dirfd(d), nm, &st, AT_SYMLINK_NOFOLLOW))
				die("fstatat %s", nm);
			t = IFTODT(st.st_mode);
		}
		if (t == DT_DIR) {
			int c = openat(dirfd(d), nm, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (c < 0)
				die("openat dir %s", nm);
			handle_dir(c);
		} else if (t == DT_REG && iset_add(&g_seen, e->d_ino)) {
			int mnt_id;
			u.fh.handle_bytes = MAX_HANDLE_SZ;
			if (name_to_handle_at(dirfd(d), nm, &u.fh, &mnt_id, 0))
				die("name_to_handle_at %s", nm);
			h_push(e->d_ino, &u.fh);
		}
	}
	if (errno)
		die("readdir");
	closedir(d);
}

static void m_handle(const char *mnt)
{
	int mfd = open(mnt, O_RDONLY | O_DIRECTORY | O_CLOEXEC), dfd;
	if (mfd < 0 || (dfd = dup(mfd)) < 0)
		die("open %s", mnt);
	handle_dir(dfd);
	hrec *tmp = malloc((g_nh ? g_nh : 1) * sizeof *tmp);
	if (!tmp)
		die("out of memory");
	sort_hrecs(g_h, g_nh, tmp);	/* inode order, as bulkstat gives it */
	free(tmp);
	for (size_t i = 0; i < g_nh; i++) {
		struct file_handle *fh = (struct file_handle *)(g_harena + g_h[i].off);
		struct stat st;
		uint64_t n;
		int f = open_by_handle_at(mfd, fh, O_RDONLY | O_CLOEXEC);
		if (f < 0)
			die("open_by_handle_at ino %" PRIu64, g_h[i].ino);
		if (fstat(f, &st))
			die("fstat");
		n = read_stream(f, st.st_ino, (uint64_t)st.st_size);
		if (n != (uint64_t)st.st_size)
			die("ino %" PRIu64 ": short read", g_h[i].ino);
		if (!g_hash)
			add_record(st.st_ino, n, NULL);
		close(f);
	}
}

/* ---- method 2b: XFS bulkstat ---- */

typedef struct {
	uint64_t ino, size;
	uint32_t gen;
} bsrec;

/* XFS decodes FILEID_INO32_GEN | XFS_FILEID_TYPE_64FLAG as {u64 ino, u32 gen} (fs/xfs/xfs_export.c) */
static int xfs_open(int mfd, uint64_t ino, uint32_t gen, int flags)
{
	union {
		struct file_handle fh;
		uint8_t raw[sizeof(struct file_handle) + 16];
	} u;
	u.fh.handle_bytes = 12;
	u.fh.handle_type = 0x81;
	memcpy(u.fh.f_handle, &ino, 8);
	memcpy(u.fh.f_handle + 8, &gen, 4);
	return open_by_handle_at(mfd, &u.fh, flags | O_CLOEXEC);
}

static void m_bulkstat(const char *mnt)
{
	enum { N = 1024 };
	int mfd = open(mnt, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	struct xfs_bulkstat_req *req = calloc(1, sizeof *req + N * sizeof(struct xfs_bulkstat));
	bsrec *dirs = NULL, *files = NULL;
	size_t nd = 0, nf = 0, cd = 0, cf = 0;
	uint64_t next = 0;
	if (mfd < 0)
		die("open %s", mnt);
	if (!req)
		die("out of memory");
	for (;;) {
		memset(&req->hdr, 0, sizeof req->hdr);
		req->hdr.ino = next;
		req->hdr.icount = N;
		if (ioctl(mfd, XFS_IOC_BULKSTAT, req))
			die("XFS_IOC_BULKSTAT (not XFS?)");
		if (!req->hdr.ocount)
			break;
		next = req->hdr.ino;
		for (uint32_t i = 0; i < req->hdr.ocount; i++) {
			const struct xfs_bulkstat *b = &req->bulkstat[i];
			bsrec r = { b->bs_ino, b->bs_size, b->bs_gen };
			if (S_ISDIR(b->bs_mode)) {
				if (nd == cd && !(dirs = realloc(dirs, (cd = cd ? cd * 2 : 1024) * sizeof *dirs)))
					die("out of memory");
				dirs[nd++] = r;
			} else if (S_ISREG(b->bs_mode) && b->bs_nlink) {
				if (nf == cf && !(files = realloc(files, (cf = cf ? cf * 2 : 4096) * sizeof *files)))
					die("out of memory");
				files[nf++] = r;
			}
		}
	}
	/* directories first, as dump and xfsdump do; opened by handle, no path walk */
	for (size_t i = 0; i < nd; i++) {
		int f = xfs_open(mfd, dirs[i].ino, dirs[i].gen, O_RDONLY | O_DIRECTORY);
		DIR *d;
		struct dirent *e;
		if (f < 0 || !(d = fdopendir(f)))
			die("open dir ino %" PRIu64, dirs[i].ino);
		for (errno = 0; (e = readdir(d)); errno = 0)
			if (!dot_or_dotdot(e->d_name))
				g_names++;
		if (errno)
			die("readdir");
		closedir(d);
	}
	for (size_t i = 0; i < nf; i++) {
		uint64_t n;
		int f = xfs_open(mfd, files[i].ino, files[i].gen, O_RDONLY);
		if (f < 0)
			die("open ino %" PRIu64, files[i].ino);
		n = read_stream(f, files[i].ino, files[i].size);
		if (n != files[i].size)
			die("ino %" PRIu64 ": short read", files[i].ino);
		if (!g_hash)
			add_record(files[i].ino, n, NULL);
		close(f);
	}
}

/* ---- method 3: raw parse. Shared: extent lists and the data transfer ---- */

typedef struct {
	uint64_t lblk, pblk;
	uint32_t len;
	uint32_t uninit;
} ext_t;

typedef struct {
	uint64_t ino, size;
	size_t first, n;	/* range in g_ex */
} fent;

static ext_t *g_ex;
static size_t g_nex, g_capex;
static fent *g_fl, *g_dl;
static size_t g_nfl, g_capfl, g_ndl, g_capdl;

static void ex_push(uint64_t lblk, uint64_t pblk, uint32_t len, uint32_t uninit)
{
	if (g_nex == g_capex) {
		g_capex = g_capex ? g_capex * 2 : 16384;
		g_ex = realloc(g_ex, g_capex * sizeof *g_ex);
		if (!g_ex)
			die("out of memory");
	}
	g_ex[g_nex++] = (ext_t){ lblk, pblk, len, uninit };
}

static void fent_push(fent **v, size_t *n, size_t *cap, fent f)
{
	if (*n == *cap) {
		*cap = *cap ? *cap * 2 : 4096;
		*v = realloc(*v, *cap * sizeof **v);
		if (!*v)
			die("out of memory");
	}
	(*v)[(*n)++] = f;
}

/*
 * Regular inodes the filesystem keeps for itself and no directory names:
 * journal, quota files, snapshot, and the orphan file (e2fsprogs >= 1.47
 * creates one by default). A raw scan sees them; the VFS never does.
 */
static uint32_t g_internal[6];

static int is_internal(uint64_t ino)
{
	for (int i = 0; i < 6; i++)
		if (g_internal[i] && g_internal[i] == ino)
			return 1;
	return 0;
}

static void pread_full(int fd, void *buf, size_t n, uint64_t off)
{
	uint8_t *p = buf;
	while (n) {
		ssize_t r = pread(fd, p, n, (off_t)off);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			die("pread at %" PRIu64, off);
		}
		if (r == 0) {
			errno = 0;
			die("unexpected end of device at %" PRIu64, off);
		}
		p += r;
		n -= (size_t)r;
		off += (uint64_t)r;
	}
}

/* Bytes of a file that live in initialised extents inside EOF: what reading it delivers. */
static uint64_t data_bytes(uint32_t bs, const fent *f)
{
	uint64_t need = 0;
	for (size_t i = 0; i < f->n; i++) {
		const ext_t *e = &g_ex[f->first + i];
		uint64_t a = e->lblk * bs, b = (e->lblk + e->len) * bs;
		if (!e->uninit && a < f->size)
			need += (b < f->size ? b : f->size) - a;
	}
	return need;
}

/*
 * Read one file's extents in logical order, merging runs that are contiguous
 * on disk. Holes and uninitialised extents are never read; the hash merge
 * counts them as zeros.
 */
static void xfer_file(int fd, uint32_t bs, const fent *f)
{
	const ext_t *ex = g_ex + f->first;
	uint64_t size = f->size;
	uint32_t fid = 0;
	size_t i = 0;
	if (g_hash) {
		uint64_t t0 = now_ns();	/* an empty or all-hole file is hashed here */
		fid = hf_open(f->ino, size, data_bytes(bs, f));
		g_read_ns += now_ns() - t0;
	}
	while (i < f->n) {
		uint64_t lstart = ex[i].lblk * bs, end;
		if (lstart >= size)
			break;
		if (ex[i].uninit) {
			i++;
			continue;
		}
		uint64_t lb = ex[i].lblk, pb = ex[i].pblk, nb = ex[i].len;
		size_t j = i + 1;
		while (j < f->n && !ex[j].uninit && ex[j].lblk == lb + nb && ex[j].pblk == pb + nb)
			nb += ex[j++].len;
		end = (lb + nb) * bs;
		end = end < size ? end : size;
		uint64_t pos = lstart, off = pb * bs;
		while (pos < end) {
			size_t chunk = end - pos < BUFSZ ? (size_t)(end - pos) : BUFSZ;
			uint64_t t0 = now_ns();
			uint8_t *buf = hb_buf(chunk);
			pread_full(fd, buf, chunk, off);
			if (g_hash) {
				deliver(fid, pos, buf, chunk);
				hb_used(chunk);
			}
			g_read_ns += now_ns() - t0;
			off += chunk;
			pos += chunk;
		}
		i = j;
	}
	if (!g_hash)
		add_record(f->ino, size, NULL);
}

static void xfer_all(int fd, uint32_t bs)
{
	for (size_t i = 0; i < g_nfl; i++)
		xfer_file(fd, bs, &g_fl[i]);
}

/* ---- method 3a: libext2fs (dump's metadata path) ---- */

static int e2_dir_cb(ext2_ino_t dir, int entry, struct ext2_dir_entry *de, int offset,
		     int blocksize, char *buf, void *priv)
{
	(void)dir, (void)offset, (void)blocksize, (void)buf;
	if (entry == DIRENT_OTHER_FILE && de->inode)
		++*(uint64_t *)priv;
	return 0;
}

static void m_e2fs(const char *dev)
{
	ext2_filsys fs;
	ext2_inode_scan scan;
	struct ext2_inode inode;
	ext2_ino_t ino;
	errcode_t err;
	int fd;
	if ((err = ext2fs_open(dev, EXT2_FLAG_64BITS, 0, 0, unix_io_manager, &fs)))
		die("ext2fs_open %s: error %ld", dev, (long)err);
	uint32_t bs = fs->blocksize;
	struct ext2_super_block *sp = fs->super;
	uint32_t internal[6] = { sp->s_journal_inum, sp->s_usr_quota_inum, sp->s_grp_quota_inum,
				 sp->s_prj_quota_inum, sp->s_orphan_file_inum, sp->s_snapshot_inum };
	memcpy(g_internal, internal, sizeof internal);
	/* 1 MiB of inode table per read, the same as the hand-written parser */
	if ((err = ext2fs_open_inode_scan(fs, (int)(BUFSZ / bs), &scan)))
		die("ext2fs_open_inode_scan: error %ld", (long)err);
	while (!(err = ext2fs_get_next_inode(scan, &ino, &inode)) && ino) {
		unsigned type = inode.i_mode & 0xF000;
		if (!inode.i_links_count)
			continue;
		if (type == 0x4000) {
			fent_push(&g_dl, &g_ndl, &g_capdl, (fent){ ino, 0, 0, 0 });
		} else if (type == 0x8000 && ino >= EXT2_FIRST_INO(fs->super) && !is_internal(ino)) {
			ext2_extent_handle_t h;
			struct ext2fs_extent e;
			fent f = { ino, EXT2_I_SIZE(&inode), g_nex, 0 };
			if (inode.i_flags & EXT4_INLINE_DATA_FL)
				die("ino %u: inline data unsupported", ino);
			if (!(inode.i_flags & EXT4_EXTENTS_FL))
				die("ino %u: block-mapped files unsupported", ino);
			if ((err = ext2fs_extent_open2(fs, ino, &inode, &h)))
				die("ext2fs_extent_open2 %u: error %ld", ino, (long)err);
			err = ext2fs_extent_get(h, EXT2_EXTENT_ROOT, &e);
			while (!err) {
				if (e.e_flags & EXT2_EXTENT_FLAGS_LEAF)
					ex_push(e.e_lblk, e.e_pblk, e.e_len, !!(e.e_flags & EXT2_EXTENT_FLAGS_UNINIT));
				err = ext2fs_extent_get(h, EXT2_EXTENT_NEXT, &e);
			}
			if (err != EXT2_ET_EXTENT_NO_NEXT && err != EXT2_ET_NO_CURRENT_NODE)
				die("ext2fs_extent_get %u: error %ld", ino, (long)err);
			ext2fs_extent_free(h);
			f.n = g_nex - f.first;
			fent_push(&g_fl, &g_nfl, &g_capfl, f);
		}
	}
	if (err)
		die("ext2fs_get_next_inode: error %ld", (long)err);
	ext2fs_close_inode_scan(scan);
	for (size_t i = 0; i < g_ndl; i++)
		if ((err = ext2fs_dir_iterate2(fs, (ext2_ino_t)g_dl[i].ino, 0, NULL, e2_dir_cb, &g_names)))
			die("ext2fs_dir_iterate2 %" PRIu64 ": error %ld", g_dl[i].ino, (long)err);
	if ((fd = open(dev, O_RDONLY | O_CLOEXEC)) < 0)
		die("open %s", dev);
	xfer_all(fd, bs);
	close(fd);
	ext2fs_close_free(&fs);
}

/* ---- method 3b: hand-written ext4 reader ---- */

static inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void walk_extents(int fd, uint32_t bs, const uint8_t *node, size_t nodesz, int level)
{
	uint16_t entries = le16(node + 2), depth = le16(node + 6);
	if (le16(node) != 0xF30A || 12 + (size_t)entries * 12 > nodesz || level > 5) {
		errno = 0;
		die("bad extent node");
	}
	for (unsigned i = 0; i < entries; i++) {
		const uint8_t *e = node + 12 + 12 * i;
		if (depth == 0) {
			uint32_t len = le16(e + 4), uninit = len > 32768;
			if (uninit)
				len -= 32768;
			ex_push(le32(e), (uint64_t)le16(e + 6) << 32 | le32(e + 8), len, uninit);
		} else {
			uint8_t *child = malloc(bs);
			if (!child)
				die("out of memory");
			pread_full(fd, child, bs, ((uint64_t)le16(e + 8) << 32 | le32(e + 4)) * bs);
			walk_extents(fd, bs, child, bs, level + 1);
			free(child);
		}
	}
}

/* Passes 1 and 2 (inode tables, directories); leaves the device open for the data. */
static int raw_parse(const char *dev, uint32_t *bs_out)
{
	uint8_t sb[1024];
	int fd = open(dev, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		die("open %s", dev);
	pread_full(fd, sb, sizeof sb, 1024);
	errno = 0;
	if (le16(sb + 56) != 0xEF53)
		die("%s: not ext2/3/4", dev);
	uint32_t incompat = le32(sb + 96), ro_compat = le32(sb + 100);
	if (incompat & 0x10)
		die("meta_bg unsupported");
	if (!(incompat & 0x40))
		die("extents feature required");
	uint32_t bs = 1024u << le32(sb + 24), ipg = le32(sb + 40), bpg = le32(sb + 32);
	uint32_t first_data = le32(sb + 20), isz = le32(sb + 76) ? le16(sb + 88) : 128;
	uint32_t first_ino = le32(sb + 76) ? le32(sb + 84) : 11;
	uint64_t blocks = le32(sb + 4) | (incompat & 0x80 ? (uint64_t)le32(sb + 336) << 32 : 0);
	uint32_t descsz = incompat & 0x80 ? le16(sb + 254) : 32;
	int csum = (ro_compat & (0x10 | 0x400)) != 0;	/* GDT_CSUM or METADATA_CSUM: itable_unused is valid */
	uint64_t groups = (blocks - first_data + bpg - 1) / bpg;
	static const unsigned internal_off[6] = { 224, 576, 580, 620, 640, 384 };
	for (int i = 0; i < 6; i++)
		g_internal[i] = le32(sb + internal_off[i]);

	size_t gdsz = groups * descsz;
	uint8_t *gd = malloc(gdsz), *it = malloc(BUFSZ);
	if (!gd || !it)
		die("out of memory");
	pread_full(fd, gd, gdsz, (uint64_t)(first_data + 1) * bs);

	/* pass 1: inode tables, one sequential sweep, 1 MiB at a time */
	for (uint64_t g = 0; g < groups; g++) {
		const uint8_t *d = gd + g * descsz;
		uint64_t itab = le32(d + 8) | (descsz >= 64 ? (uint64_t)le32(d + 0x28) << 32 : 0);
		uint32_t unused = le16(d + 0x1C) | (descsz >= 64 ? (uint32_t)le16(d + 0x32) << 16 : 0);
		uint32_t used = ipg;
		if (csum) {
			if (le16(d + 0x12) & 1)	/* INODE_UNINIT */
				continue;
			used = ipg - unused;
		}
		uint64_t off = itab * bs, left = (uint64_t)used * isz;
		uint32_t idx = 0;
		while (left) {
			size_t chunk = left < BUFSZ ? (size_t)left : BUFSZ;
			pread_full(fd, it, chunk, off);
			for (size_t p = 0; p < chunk; p += isz, idx++) {
				const uint8_t *in = it + p;
				uint64_t ino = g * ipg + idx + 1;
				unsigned type = le16(in) & 0xF000;
				if (!le16(in + 26))
					continue;
				if (type != 0x4000 && !(type == 0x8000 && ino >= first_ino && !is_internal(ino)))
					continue;
				uint32_t fl = le32(in + 32);
				if (fl & 0x10000000) {
					errno = 0;
					die("ino %" PRIu64 ": inline data unsupported", ino);
				}
				if (!(fl & 0x80000)) {
					errno = 0;
					die("ino %" PRIu64 ": block-mapped files unsupported", ino);
				}
				fent f = { ino, le32(in + 4) | (uint64_t)le32(in + 108) << 32, g_nex, 0 };
				walk_extents(fd, bs, in + 40, 60, 0);
				f.n = g_nex - f.first;
				if (type == 0x4000)
					fent_push(&g_dl, &g_ndl, &g_capdl, f);
				else
					fent_push(&g_fl, &g_nfl, &g_capfl, f);
			}
			off += chunk;
			left -= chunk;
		}
	}
	free(it);
	free(gd);

	/* pass 2: directories, linear walk of every block (htree nodes hide as empty entries) */
	for (size_t i = 0; i < g_ndl; i++) {
		const ext_t *ex = g_ex + g_dl[i].first;
		for (size_t k = 0; k < g_dl[i].n; k++) {
			if (ex[k].uninit)
				continue;
			uint64_t left = (uint64_t)ex[k].len * bs, off = ex[k].pblk * bs;
			while (left) {
				size_t chunk = left < BUFSZ ? (size_t)left : BUFSZ;
				pread_full(fd, g_buf, chunk, off);
				for (size_t b = 0; b < chunk; b += bs) {
					const uint8_t *blk = g_buf + b;
					for (uint32_t o = 0; o + 8 <= bs;) {
						uint32_t din = le32(blk + o);
						uint16_t rl = le16(blk + o + 4);
						uint8_t nl = blk[o + 6];
						if (rl < 8 || o + rl > bs) {
							errno = 0;
							die("dir ino %" PRIu64 ": bad rec_len", g_dl[i].ino);
						}
						if (din && !(nl == 1 && blk[o + 8] == '.') &&
						    !(nl == 2 && blk[o + 8] == '.' && blk[o + 9] == '.'))
							g_names++;
						o += rl;
					}
				}
				off += chunk;
				left -= chunk;
			}
		}
	}

	*bs_out = bs;
	return fd;
}

static void m_raw(const char *dev)
{
	uint32_t bs;
	int fd = raw_parse(dev, &bs);
	xfer_all(fd, bs);	/* pass 3: file data in inode order, file by file */
	close(fd);
}

/* ---- method 3c: hand-written ext4 reader, every extent of every file in physical order ---- */

typedef struct {
	uint64_t pblk;
	uint32_t nblk;	/* at most BUFSZ / bs */
	uint32_t file;	/* index into g_fl */
	uint32_t ext;	/* index into g_ex */
	uint32_t off;	/* first block of this piece within its extent */
} piece_t;

#define PBLK(x) ((x).pblk)
RADIX_DEFINE(sort_pieces, piece_t, PBLK)

static void m_rawsort(const char *dev)
{
	uint32_t bs;
	int fd = raw_parse(dev, &bs);
	uint32_t maxblk = BUFSZ / bs;
	piece_t *pc = NULL, *tmp;
	size_t np = 0, cap = 0;

	/* plan: every initialised extent inside EOF, cut to <= 1 MiB, sorted by disk address */
	for (size_t fi = 0; fi < g_nfl; fi++) {
		const fent *f = &g_fl[fi];
		for (size_t k = 0; k < f->n; k++) {
			const ext_t *e = &g_ex[f->first + k];
			if (e->uninit)
				continue;
			for (uint32_t off = 0; off < e->len && (e->lblk + off) * bs < f->size; off += maxblk) {
				if (np == cap && !(pc = realloc(pc, (cap = cap ? cap * 2 : 65536) * sizeof *pc)))
					die("out of memory");
				uint32_t nb = e->len - off < maxblk ? e->len - off : maxblk;
				pc[np++] = (piece_t){ e->pblk + off, nb, (uint32_t)fi, (uint32_t)(f->first + k), off };
			}
		}
	}
	const char *dump = getenv("ACCBENCH_PLAN_DUMP");
	if (dump) {	/* for bench/levelup/sort: the unsorted plan, then stop */
		FILE *f = fopen(dump, "wb");
		if (!f || fwrite(pc, sizeof *pc, np, f) != np || fclose(f))
			die("%s", dump);
		exit(0);
	}
	if (!(tmp = malloc((np ? np : 1) * sizeof *tmp)))
		die("out of memory");
	sort_pieces(pc, np, tmp);
	free(tmp);
	if (g_hash) {
		uint64_t t0 = now_ns();	/* empty and all-hole files are hashed here, as in raw */
		for (size_t fi = 0; fi < g_nfl; fi++)
			hf_open(g_fl[fi].ino, g_fl[fi].size, data_bytes(bs, &g_fl[fi]));	/* fid == fi */
		g_read_ns += now_ns() - t0;
	}

	/*
	 * read: one pread per run of physically adjacent pieces, whichever files
	 * they belong to. Each piece goes to its file's hash as it arrives: no
	 * piece waits for another, whatever order the disk gives them in.
	 */
	for (size_t i = 0; i < np;) {
		uint64_t start = pc[i].pblk, nb = pc[i].nblk;
		size_t j = i + 1;
		while (j < np && pc[j].pblk == start + nb && nb + pc[j].nblk <= maxblk)
			nb += pc[j++].nblk;
		uint64_t t0 = now_ns();
		uint8_t *buf = hb_buf(nb * bs);
		pread_full(fd, buf, nb * bs, start * bs);
		if (g_hash) {
			for (size_t k = i; k < j; k++) {
				const fent *f = &g_fl[pc[k].file];
				uint64_t lstart = (g_ex[pc[k].ext].lblk + pc[k].off) * bs;
				uint64_t n = (uint64_t)pc[k].nblk * bs;
				if (n > f->size - lstart)
					n = f->size - lstart;	/* the file ends inside this piece */
				deliver(pc[k].file, lstart, buf + (pc[k].pblk - start) * bs, n);
			}
			hb_used(nb * bs);
		}
		g_read_ns += now_ns() - t0;
		i = j;
	}
	if (!g_hash)
		for (size_t fi = 0; fi < g_nfl; fi++)
			add_record(g_fl[fi].ino, g_fl[fi].size, NULL);
	free(pc);
	close(fd);
}

/* ---- main ---- */

static const char *CSV_HEADER =
	"impl,method,files,names,bytes,find_ns,read_ns,total_ns,utime_us,stime_us,"
	"minflt,majflt,inblock,nvcsw,nivcsw,maxrss_kb,combined";

int main(int argc, char **argv)
{
	const char *method = NULL, *target = NULL, *digest = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--header")) {
			puts(CSV_HEADER);
			return 0;
		} else if (!strcmp(argv[i], "--hash")) {
			g_hash = 1;
		} else if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
			g_threads = (unsigned)atoi(argv[++i]);
			if (g_threads < 1 || g_threads > 1024) {
				method = NULL;
				break;
			}
		} else if (!strcmp(argv[i], "--digest") && i + 1 < argc) {
			digest = argv[++i];
			g_hash = 1;
		} else if (!method) {
			method = argv[i];
		} else if (!target) {
			target = argv[i];
		} else {
			method = NULL;
			break;
		}
	}
	if (!method || !target) {
		fprintf(stderr, "usage: accbench {vfs|handle|bulkstat|e2fs|raw|rawsort} TARGET [--hash] [--digest FILE]\n"
				"                [--threads N]\n"
				"       accbench --header\n");
		return 2;
	}
	if (posix_memalign((void **)&g_buf, 4096, BUFSZ))
		die("out of memory");
	memset(g_buf, 0, BUFSZ);	/* fault the buffer in before timing */
	if (g_threads > 1 && g_hash) {	/* the pool's threads start before timing too */
		g_arena_sz = (size_t)2 * g_threads * BUFSZ;
		if (posix_memalign((void **)&g_arena, 4096, g_arena_sz) || !(g_pool = pool_new(g_threads)))
			die("out of memory");
		memset(g_arena, 0, g_arena_sz);
	}

	uint64_t t0 = now_ns();
	if (!strcmp(method, "vfs"))
		m_vfs(target);
	else if (!strcmp(method, "handle"))
		m_handle(target);
	else if (!strcmp(method, "bulkstat"))
		m_bulkstat(target);
	else if (!strcmp(method, "e2fs"))
		m_e2fs(target);
	else if (!strcmp(method, "raw"))
		m_raw(target);
	else if (!strcmp(method, "rawsort"))
		m_rawsort(target);
	else {
		errno = 0;
		die("unknown method %s", method);
	}
	if (g_hash) {
		uint64_t t1 = now_ns();
		hb_flush();	/* the last batch, and the files it completes */
		g_read_ns += now_ns() - t1;
	}
	uint64_t total = now_ns() - t0;
	for (size_t i = 0; i < g_nhf; i++)
		if (!g_hf[i].done) {
			errno = 0;
			die("ino %" PRIu64 ": data never completed", g_hf[i].ino);
		}

	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	printf("%s,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
	       ",%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%016" PRIx64 "\n",
	       IMPL, method, g_files, g_names, g_bytes, total - g_read_ns, g_read_ns, total,
	       (long)(ru.ru_utime.tv_sec * 1000000 + ru.ru_utime.tv_usec),
	       (long)(ru.ru_stime.tv_sec * 1000000 + ru.ru_stime.tv_usec),
	       ru.ru_minflt, ru.ru_majflt, ru.ru_inblock, ru.ru_nvcsw, ru.ru_nivcsw, ru.ru_maxrss, g_combined);

	if (digest) {
		FILE *o = fopen(digest, "w");
		if (!o)
			die("fopen %s", digest);
		rec_t *tmp = malloc((g_nrecs ? g_nrecs : 1) * sizeof *tmp);
		if (!tmp)
			die("out of memory");
		sort_recs(g_recs, g_nrecs, tmp);
		free(tmp);
		fprintf(o, "# files=%" PRIu64 " names=%" PRIu64 " bytes=%" PRIu64 "\n", g_files, g_names, g_bytes);
		for (size_t i = 0; i < g_nrecs; i++) {
			fprintf(o, "%" PRIu64 " %" PRIu64 " ", g_recs[i].ino, g_recs[i].size);
			for (unsigned k = 0; k < B3_OUT; k++)
				fprintf(o, "%02x", g_recs[i].digest[k]);
			fputc('\n', o);
		}
		if (fclose(o))
			die("write %s", digest);
	}
	return 0;
}
