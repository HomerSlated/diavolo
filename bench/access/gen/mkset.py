#!/usr/bin/env python3
"""Deterministic file-set generator for accbench.

    mkset.py KIND DEST --layout fresh|aged [--content random|text] [--seed N]
             [--scale F] [--from DIR]

KIND is the size class:
  huge   one 2 GiB file
  tiny   100,000 files of 0-4 KiB in 256 directories
  small  30,000 files of 4-64 KiB in 256 directories
  mixed  ~1.5 GiB of mostly small, some medium, a few large files in a random
         tree, plus sparse files, hardlinks and symlinks
  real   a copy of --from DIR (sizes, names and tree shape of real data)

--layout decides where the data lands on disk; the bytes are the same either way:
  fresh  every file written whole, in creation order: inode order is disk order
  aged   all inodes created first, then data written in shuffled order with
         large files interleaved and synced chunk by chunk; 30% extra filler
         files are written alongside and then deleted, and 20% of the files
         are rewritten into the holes they leave. Inode order stops matching
         disk order and large files fragment.

--content (not used by 'real'):
  random  incompressible
  text    word-like and highly compressible

Same arguments give the same logical tree on any filesystem, so the ext4 and
XFS images of a set hold identical data and differ only in the allocator.
"""

import argparse
import ctypes
import os
import random
import sys

MiB = 1 << 20
POOL = 64 * MiB
WORDS = (b"the of and to in is was for on that with as by at from it an be this which or "
         b"are backup dump restore inode extent block directory file archive level index "
         b"filesystem mount device kernel buffer cache read write sector stream header").split()


_libc = ctypes.CDLL(None, use_errno=True)


def syncfs(dest):
    """Flush only the filesystem under test, not the whole machine."""
    fd = os.open(dest, os.O_RDONLY | os.O_DIRECTORY)
    try:
        if _libc.syncfs(fd) != 0:
            raise OSError(ctypes.get_errno(), "syncfs")
    finally:
        os.close(fd)


class Spec:
    __slots__ = ("path", "size", "islands", "src", "key")

    def __init__(self, path, size, key, islands=None, src=None):
        self.path, self.size, self.key = path, size, key
        self.islands = islands  # sparse: [(offset, length)], else None
        self.src = src          # real: source file path


def make_pool(r, content):
    if content == "random":
        return r.randbytes(POOL)
    out, n = [], 0
    while n < 8 * MiB:
        line = b" ".join(r.choice(WORDS) for _ in range(r.randint(6, 14))) + b".\n"
        out.append(line)
        n += len(line)
    tile = b"".join(out)[:8 * MiB]
    return tile * (POOL // len(tile))


def dirs_256():
    return [f"d{a:x}/d{b:x}" for a in range(16) for b in range(16)]


def plan(kind, r, scale, src_root):
    """Returns (files, extras): files are Specs; extras are ('link'|'symlink', a, b)."""
    files, extras = [], []
    if kind == "huge":
        files.append(Spec("huge.bin", int(2048 * MiB * scale), 0))
    elif kind in ("tiny", "small"):
        n = int((100_000 if kind == "tiny" else 30_000) * scale)
        lo, hi = (0, 4095) if kind == "tiny" else (4096, 65536)
        ds = dirs_256()
        for i in range(n):
            files.append(Spec(f"{ds[i % 256]}/f{i:06d}", r.randint(lo, hi), i))
    elif kind == "mixed":
        budget, i = int(1536 * MiB * scale), 0
        ds = [""]
        while budget > 0:
            if len(ds) < 400 and (r.random() < 0.02 or len(ds) < 8):
                parent = r.choice(ds)
                if parent.count("/") < 3:
                    ds.append(f"{parent}/s{len(ds)}".lstrip("/"))
            u = r.random()
            e = r.uniform(0, 16) if u < 0.80 else r.uniform(16, 22) if u < 0.99 else r.uniform(22, 26)
            size = int(2 ** e) - 1
            d = r.choice(ds)
            path = f"{d}/m{i:05d}".lstrip("/")
            if r.random() < 0.01:  # sparse: 1-4 data islands in 8-64 MiB of hole
                size = r.randint(8, 64) * MiB
                isl = sorted((r.randrange(0, size - 65536) & ~4095, r.randint(1, 65536)) for _ in range(r.randint(1, 4)))
                files.append(Spec(path, size, i, islands=isl))
                budget -= sum(x[1] for x in isl)
            else:
                files.append(Spec(path, size, i))
                budget -= size
            i += 1
        for f in r.sample(files, max(1, len(files) // 50)):
            extras.append(("link", f.path, f"{r.choice(ds)}/hl{len(extras)}".lstrip("/")))
        for f in r.sample(files, max(1, len(files) // 100)):
            extras.append(("symlink", "/" + f.path, f"{r.choice(ds)}/sl{len(extras)}".lstrip("/")))
    elif kind == "real":
        seen = {}
        i = 0
        for root, dnames, fnames in os.walk(src_root):
            dnames.sort()
            for fn in sorted(fnames):
                full = os.path.join(root, fn)
                rel = os.path.relpath(full, src_root)
                st = os.lstat(full)
                if os.path.islink(full):
                    extras.append(("symlink", os.readlink(full), rel))
                elif os.path.isfile(full):
                    k = (st.st_dev, st.st_ino)
                    if k in seen:
                        extras.append(("link", seen[k], rel))
                    else:
                        seen[k] = rel
                        files.append(Spec(rel, st.st_size, i, src=full))
                        i += 1
            for dn in dnames:  # keep empty directories
                full = os.path.join(root, dn)
                if os.path.islink(full):
                    extras.append(("symlink", os.readlink(full), os.path.relpath(full, src_root)))
        dirs = {os.path.relpath(os.path.join(rt, d), src_root)
                for rt, dn, _ in os.walk(src_root) for d in dn
                if not os.path.islink(os.path.join(rt, d))}
        return files, extras, sorted(dirs)
    else:
        sys.exit(f"mkset: unknown kind {kind}")
    return files, extras, None


class Writer:
    def __init__(self, dest, pool):
        self.dest, self.pool = dest, pool
        self.srcfd = {}

    def content(self, spec, off, n):
        """Bytes [off, off+n) of spec's logical content."""
        if spec.src:
            with open(spec.src, "rb") as f:
                f.seek(off)
                b = f.read(n)
        else:
            start = (spec.key * 7919 * 4096 + off) % POOL
            b = self.pool[start:start + n] if start + n <= POOL else self.pool[start:] + self.pool[:n - (POOL - start)]
        if len(b) != n:  # a short slice would silently shrink a file
            sys.exit(f"mkset: {spec.path}: wanted {n} bytes at {off}, got {len(b)}")
        return b

    def write_whole(self, spec, fd):
        if spec.islands:
            for off, n in spec.islands:
                os.pwrite(fd, self.content(spec, off, n), off)
            os.ftruncate(fd, spec.size)
            return
        off = 0
        while off < spec.size:
            n = min(8 * MiB, spec.size - off)
            os.pwrite(fd, self.content(spec, off, n), off)
            off += n


def create_all(dest, files, dirs):
    for d in dirs or []:
        os.makedirs(os.path.join(dest, d), exist_ok=True)
    for f in files:
        p = os.path.join(dest, f.path)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        os.close(os.open(p, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644))


def write_fresh(w, dest, files):
    for f in files:
        fd = os.open(os.path.join(dest, f.path), os.O_WRONLY)
        w.write_whole(f, fd)
        os.close(fd)


def write_aged(w, dest, files, r):
    """Shuffled, interleaved, synced writes: allocation follows write order, not inode order."""
    order = files[:]
    r.shuffle(order)
    pending = []  # (spec, fd, offset)
    since_sync = 0
    i = 0
    while i < len(order) or pending:
        while len(pending) < 32 and i < len(order):
            f = order[i]
            i += 1
            fd = os.open(os.path.join(dest, f.path), os.O_WRONLY)
            if f.islands or f.size <= 256 * 1024:
                w.write_whole(f, fd)  # small or sparse: one go
                os.close(fd)
                since_sync += f.size if not f.islands else 0
            else:
                pending.append([f, fd, 0])
        if pending:
            j = r.randrange(len(pending))
            f, fd, off = pending[j]
            n = min(r.randint(64 * 1024, 1024 * 1024), f.size - off)
            os.pwrite(fd, w.content(f, off, n), off)
            os.fsync(fd)  # allocate this chunk now, next to whatever else was just written
            pending[j][2] = off + n
            if off + n >= f.size:
                os.close(fd)
                pending.pop(j)
        if since_sync >= 4 * MiB:
            syncfs(dest)
            since_sync = 0
    syncfs(dest)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kind", choices=["huge", "tiny", "small", "mixed", "real"])
    ap.add_argument("dest")
    ap.add_argument("--layout", choices=["fresh", "aged"], required=True)
    ap.add_argument("--content", choices=["random", "text"], default="random")
    ap.add_argument("--seed", type=int, default=20260927)
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--from", dest="src", default="/usr/share")
    a = ap.parse_args()

    r = random.Random(f"{a.seed}:{a.kind}")  # tree shape: independent of content
    files, extras, dirs = plan(a.kind, r, a.scale, a.src)
    # built for 'real' too: its filler files take their bytes from the pool
    pool = make_pool(random.Random(f"{a.seed}:{a.content}"), a.content)
    w = Writer(a.dest, pool)
    lr = random.Random(f"{a.seed}:{a.kind}:layout")

    create_all(a.dest, files, dirs)
    if a.layout == "fresh":
        write_fresh(w, a.dest, files)
    else:
        # fillers: 30% more files of the same shapes, created and written alongside, then deleted
        fillers = [Spec(f".filler/{k // 1000}/x{k}", f.size, 10_000_000 + k)
                   for k, f in enumerate(lr.sample(files, max(1, len(files) * 3 // 10)))]
        if a.kind == "huge":
            fillers = [Spec(".filler/0/x0", files[0].size, 10_000_000)]
        create_all(a.dest, fillers, None)
        write_aged(w, a.dest, files + fillers, lr)
        for f in fillers:
            os.unlink(os.path.join(a.dest, f.path))
        for root, dn, _ in os.walk(os.path.join(a.dest, ".filler"), topdown=False):
            os.rmdir(root)
        syncfs(a.dest)
        rewrite = lr.sample(files, max(1, len(files) // 5)) if a.kind != "huge" else []
        for f in rewrite:
            os.truncate(os.path.join(a.dest, f.path), 0)
        syncfs(a.dest)
        write_aged(w, a.dest, rewrite, lr)

    for op, x, y in extras:
        p = os.path.join(a.dest, y)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        if op == "link":
            os.link(os.path.join(a.dest, x), p)
        else:
            os.symlink(x, p)
    syncfs(a.dest)
    total = sum(f.size for f in files)
    print(f"mkset: {a.kind} {a.layout} {a.content}: {len(files)} files, {len(extras)} links, "
          f"{total / MiB:.1f} MiB logical")


if __name__ == "__main__":
    main()
