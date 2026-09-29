/*
 * accbench — storage access-method benchmark, C implementation.
 *
 * Reads every regular file on one filesystem by one access method and reports
 * how long it spent finding the data versus reading it. The Rust program in
 * ../rust is a line-for-line peer: same methods, same syscalls, same buffer
 * size, same hash, same output. See ../README.md.
 *
 *   accbench vfs      MOUNTPOINT   method 1: lstat + open + read by path
 *   accbench handle   MOUNTPOINT   method 2: walk, name_to_handle_at, inode order, open_by_handle_at
 *   accbench bulkstat MOUNTPOINT   method 2: XFS_IOC_BULKSTAT + open_by_handle_at (xfsdump's way)
 *   accbench e2fs     DEVICE       method 3: libext2fs inode scan, own pread of data (dump's way)
 *   accbench raw      DEVICE       method 3: hand-written ext4 parser, own pread of data
 *   accbench rawsort  DEVICE       method 3: as raw, but all extents of all files in disk order
 *
 * Options: --hash (hash all file content), --digest FILE (implies --hash; write
 * "ino size digest" per file, sorted by inode), --header (print CSV header only).
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

/* ---- content hash (identical in Rust; not cryptographic, just a fingerprint) ---- */

typedef struct {
	uint64_t h, len, pend;
	unsigned npend;
} hstate;

static inline uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static inline uint64_t absorb(uint64_t h, uint64_t w) { return rotl(h ^ w, 27) * HK; }

static inline uint64_t fmix(uint64_t h)
{
	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdull;
	h ^= h >> 33;
	h *= 0xc4ceb9fe1a85ec53ull;
	h ^= h >> 33;
	return h;
}

static void h_init(hstate *s)
{
	s->h = 0x243F6A8885A308D3ull;
	s->len = s->pend = 0;
	s->npend = 0;
}

static void h_update(hstate *s, const uint8_t *p, size_t n)
{
	uint64_t h = s->h;
	s->len += n;
	while (s->npend && n) {
		s->pend |= (uint64_t)*p++ << (8 * s->npend);
		n--;
		if (++s->npend == 8) {
			h = absorb(h, s->pend);
			s->pend = 0;
			s->npend = 0;
		}
	}
	while (n >= 8) {
		uint64_t w;
		memcpy(&w, p, 8);	/* little-endian host assumed (x86-64) */
		h = absorb(h, w);
		p += 8;
		n -= 8;
	}
	while (n) {
		s->pend |= (uint64_t)*p++ << (8 * s->npend++);
		n--;
	}
	s->h = h;
}

static void h_zeros(hstate *s, uint64_t n)
{
	static const uint8_t z[65536];
	while (n) {
		size_t k = n < sizeof z ? (size_t)n : sizeof z;
		h_update(s, z, k);
		n -= k;
	}
}

static uint64_t h_final(const hstate *s)
{
	uint64_t h = s->h;
	if (s->npend)
		h = absorb(h, s->pend);
	return fmix(h ^ s->len);
}

/* ---- per-file records ---- */

typedef struct {
	uint64_t ino, size, digest;
} rec_t;

static rec_t *g_recs;
static size_t g_nrecs, g_caprecs;

static void add_record(uint64_t ino, uint64_t size, uint64_t digest)
{
	if (g_nrecs == g_caprecs) {
		g_caprecs = g_caprecs ? g_caprecs * 2 : 4096;
		g_recs = realloc(g_recs, g_caprecs * sizeof *g_recs);
		if (!g_recs)
			die("out of memory");
	}
	g_recs[g_nrecs++] = (rec_t){ ino, size, digest };
	g_files++;
	g_bytes += size;
	if (g_hash)
		g_combined += fmix(digest ^ (ino * HK));
}

static int cmp_rec(const void *a, const void *b)
{
	uint64_t x = ((const rec_t *)a)->ino, y = ((const rec_t *)b)->ino;
	return (x > y) - (x < y);
}

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

/* ---- the shared read loop for fd-based methods (1 and 2) ---- */

static uint64_t read_stream(int fd, uint64_t *digest)
{
	hstate hs;
	uint64_t tot = 0;
	h_init(&hs);
	for (;;) {
		uint64_t t0 = now_ns();
		ssize_t r = read(fd, g_buf, BUFSZ);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			die("read");
		}
		if (g_hash && r > 0)
			h_update(&hs, g_buf, (size_t)r);
		g_read_ns += now_ns() - t0;
		if (r == 0)
			break;
		tot += (uint64_t)r;
	}
	*digest = g_hash ? h_final(&hs) : 0;
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
			uint64_t dg, n;
			int f = openat(dirfd(d), nm, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
			if (f < 0)
				die("openat %s", nm);
			n = read_stream(f, &dg);
			if (n != (uint64_t)st.st_size)
				die("%s: read %" PRIu64 " of %" PRIu64 " bytes", nm, n, (uint64_t)st.st_size);
			add_record(st.st_ino, n, dg);
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

static int cmp_hrec(const void *a, const void *b)
{
	uint64_t x = ((const hrec *)a)->ino, y = ((const hrec *)b)->ino;
	return (x > y) - (x < y);
}

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
	qsort(g_h, g_nh, sizeof *g_h, cmp_hrec);	/* inode order, as bulkstat gives it */
	for (size_t i = 0; i < g_nh; i++) {
		struct file_handle *fh = (struct file_handle *)(g_harena + g_h[i].off);
		struct stat st;
		uint64_t dg, n;
		int f = open_by_handle_at(mfd, fh, O_RDONLY | O_CLOEXEC);
		if (f < 0)
			die("open_by_handle_at ino %" PRIu64, g_h[i].ino);
		if (fstat(f, &st))
			die("fstat");
		n = read_stream(f, &dg);
		if (n != (uint64_t)st.st_size)
			die("ino %" PRIu64 ": short read", g_h[i].ino);
		add_record(st.st_ino, n, dg);
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
		uint64_t dg, n;
		int f = xfs_open(mfd, files[i].ino, files[i].gen, O_RDONLY);
		if (f < 0)
			die("open ino %" PRIu64, files[i].ino);
		n = read_stream(f, &dg);
		if (n != files[i].size)
			die("ino %" PRIu64 ": short read", files[i].ino);
		add_record(files[i].ino, n, dg);
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

/* Read one file's extents in logical order, merging runs that are contiguous on disk. */
static void xfer_file(int fd, uint32_t bs, const fent *f)
{
	const ext_t *ex = g_ex + f->first;
	uint64_t pos = 0, size = f->size;
	hstate hs;
	size_t i = 0;
	h_init(&hs);
	while (i < f->n) {
		uint64_t lstart = ex[i].lblk * bs, end;
		if (lstart >= size)
			break;
		if (lstart > pos) {	/* hole: never read, only hashed as zeros */
			if (g_hash) {
				uint64_t t0 = now_ns();
				h_zeros(&hs, lstart - pos);
				g_read_ns += now_ns() - t0;
			}
			pos = lstart;
		}
		if (ex[i].uninit) {
			end = (ex[i].lblk + ex[i].len) * bs;
			end = end < size ? end : size;
			if (g_hash) {
				uint64_t t0 = now_ns();
				h_zeros(&hs, end - pos);
				g_read_ns += now_ns() - t0;
			}
			pos = end;
			i++;
			continue;
		}
		uint64_t lb = ex[i].lblk, pb = ex[i].pblk, nb = ex[i].len;
		size_t j = i + 1;
		while (j < f->n && !ex[j].uninit && ex[j].lblk == lb + nb && ex[j].pblk == pb + nb)
			nb += ex[j++].len;
		end = (lb + nb) * bs;
		end = end < size ? end : size;
		uint64_t off = pb * bs + (pos - lb * bs);
		while (pos < end) {
			size_t chunk = end - pos < BUFSZ ? (size_t)(end - pos) : BUFSZ;
			uint64_t t0 = now_ns();
			pread_full(fd, g_buf, chunk, off);
			if (g_hash)
				h_update(&hs, g_buf, chunk);
			g_read_ns += now_ns() - t0;
			off += chunk;
			pos += chunk;
		}
		i = j;
	}
	if (pos < size && g_hash) {	/* trailing hole */
		uint64_t t0 = now_ns();
		h_zeros(&hs, size - pos);
		g_read_ns += now_ns() - t0;
	}
	add_record(f->ino, size, g_hash ? h_final(&hs) : 0);
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

typedef struct stash {
	struct stash *next;
	uint32_t ext, off;
	size_t n;
	uint8_t data[];
} stash_t;

/*
 * Per-file reassembly for hashing: the digest must see a file's bytes in
 * logical order, but physical order can deliver them out of order. In-order
 * bytes are hashed at once; early ones wait in the stash. Holes and uninit
 * extents are hashed as zeros when the cursor reaches them.
 */
typedef struct {
	hstate hs;
	uint64_t pos;	/* logical bytes hashed so far */
	uint32_t cur;	/* extent the cursor is in, relative to the file's first */
	uint32_t done;
	stash_t *stash;
} fstate;

static uint32_t g_bs;
static fstate *g_fs;
static uint64_t g_stash_bytes, g_stash_peak;

static int cmp_piece(const void *a, const void *b)
{
	uint64_t x = ((const piece_t *)a)->pblk, y = ((const piece_t *)b)->pblk;
	return (x > y) - (x < y);
}

static void fs_advance(uint32_t fi)
{
	const fent *f = &g_fl[fi];
	fstate *st = &g_fs[fi];
	while (!st->done) {
		const ext_t *e = &g_ex[f->first + st->cur];
		uint64_t lstart, lend;
		if (st->cur == f->n || (lstart = e->lblk * g_bs) >= f->size) {
			h_zeros(&st->hs, f->size - st->pos);	/* trailing hole */
			add_record(f->ino, f->size, h_final(&st->hs));
			st->done = 1;
			break;
		}
		lend = (e->lblk + e->len) * g_bs;
		lend = lend < f->size ? lend : f->size;
		if (st->pos < lstart) {	/* hole */
			h_zeros(&st->hs, lstart - st->pos);
			st->pos = lstart;
		} else if (e->uninit) {
			h_zeros(&st->hs, lend - st->pos);
			st->pos = lend;
			st->cur++;
		} else if (st->pos >= lend) {
			st->cur++;
		} else {
			break;	/* waiting for this extent's data */
		}
	}
}

static void deliver(const piece_t *p, const uint8_t *data, size_t n)
{
	fstate *st = &g_fs[p->file];
	uint32_t rel = p->ext - (uint32_t)g_fl[p->file].first;
	if (rel != st->cur || (g_ex[p->ext].lblk + p->off) * g_bs != st->pos) {
		stash_t *s = malloc(sizeof *s + n);
		if (!s)
			die("out of memory");
		s->ext = rel;
		s->off = p->off;
		s->n = n;
		memcpy(s->data, data, n);
		s->next = st->stash;
		st->stash = s;
		g_stash_bytes += n;
		if (g_stash_bytes > g_stash_peak)
			g_stash_peak = g_stash_bytes;
		return;
	}
	h_update(&st->hs, data, n);
	st->pos += n;
	fs_advance(p->file);
	for (;;) {	/* drain whatever was waiting for this */
		stash_t **pp = &st->stash, *s;
		const fent *f = &g_fl[p->file];
		while ((s = *pp) && !(s->ext == st->cur && (g_ex[f->first + s->ext].lblk + s->off) * g_bs == st->pos))
			pp = &s->next;
		if (!s)
			break;
		*pp = s->next;
		h_update(&st->hs, s->data, s->n);
		st->pos += s->n;
		g_stash_bytes -= s->n;
		free(s);
		fs_advance(p->file);
	}
}

static void m_rawsort(const char *dev)
{
	uint32_t bs;
	int fd = raw_parse(dev, &bs);
	uint32_t maxblk = BUFSZ / bs;
	piece_t *pc = NULL;
	size_t np = 0, cap = 0;
	g_bs = bs;

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
	qsort(pc, np, sizeof *pc, cmp_piece);
	if (g_hash) {
		if (!(g_fs = calloc(g_nfl, sizeof *g_fs)))
			die("out of memory");
		uint64_t t0 = now_ns();	/* hashing leading holes is read work, as in raw */
		for (uint32_t fi = 0; fi < g_nfl; fi++) {
			h_init(&g_fs[fi].hs);
			fs_advance(fi);	/* empty and all-hole files finish here */
		}
		g_read_ns += now_ns() - t0;
	}

	/* read: one pread per run of physically adjacent pieces, whichever files they belong to */
	for (size_t i = 0; i < np;) {
		uint64_t start = pc[i].pblk, nb = pc[i].nblk;
		size_t j = i + 1;
		while (j < np && pc[j].pblk == start + nb && nb + pc[j].nblk <= maxblk)
			nb += pc[j++].nblk;
		uint64_t t0 = now_ns();
		pread_full(fd, g_buf, nb * bs, start * bs);
		if (g_hash) {
			for (size_t k = i; k < j; k++) {
				const fent *f = &g_fl[pc[k].file];
				uint64_t lstart = (g_ex[pc[k].ext].lblk + pc[k].off) * bs;
				uint64_t n = (uint64_t)pc[k].nblk * bs;
				if (n > f->size - lstart)
					n = f->size - lstart;	/* the file ends inside this piece */
				deliver(&pc[k], g_buf + (pc[k].pblk - start) * bs, n);
			}
		}
		g_read_ns += now_ns() - t0;
		i = j;
	}
	if (g_hash) {
		for (size_t fi = 0; fi < g_nfl; fi++)
			if (!g_fs[fi].done) {
				errno = 0;
				die("ino %" PRIu64 ": data never completed", g_fl[fi].ino);
			}
		if (g_stash_peak)
			fprintf(stderr, "accbench: rawsort held back at most %" PRIu64 " bytes for reordering\n", g_stash_peak);
	} else {
		for (size_t fi = 0; fi < g_nfl; fi++)
			add_record(g_fl[fi].ino, g_fl[fi].size, 0);
	}
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
				"       accbench --header\n");
		return 2;
	}
	if (posix_memalign((void **)&g_buf, 4096, BUFSZ))
		die("out of memory");
	memset(g_buf, 0, BUFSZ);	/* fault the buffer in before timing */

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
	uint64_t total = now_ns() - t0;

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
		qsort(g_recs, g_nrecs, sizeof *g_recs, cmp_rec);
		fprintf(o, "# files=%" PRIu64 " names=%" PRIu64 " bytes=%" PRIu64 "\n", g_files, g_names, g_bytes);
		for (size_t i = 0; i < g_nrecs; i++)
			fprintf(o, "%" PRIu64 " %" PRIu64 " %016" PRIx64 "\n", g_recs[i].ino, g_recs[i].size, g_recs[i].digest);
		if (fclose(o))
			die("write %s", digest);
	}
	return 0;
}
