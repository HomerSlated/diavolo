# accbench — storage access methods, and Rust vs C

A standalone benchmark, deliberately outside the Diavolo workspace and spec. It
answers two questions with measurements instead of inference:

1. **Which access method reads a filesystem fastest, and where does the time go?**
2. **Does the implementation language matter?** Every method is written twice,
   in C (built with both gcc and clang) and in Rust. The two versions are
   line-for-line peers: the same syscalls, the same 1 MiB buffer, the same hash
   and the same output format. Each uses its own language's tools for the
   hash, the sort and the threads (see "Hashing" below).

## The three access methods

| # | Method | `accbench` name | Finds files by | Reads data by | Example tools |
|---|---|---|---|---|---|
| 1 | VFS | `vfs` | `readdir` walk, `lstat` per name | `open`/`read` per path | tar, rsync, blitcp |
| 2 | Kernel-cooperative | `handle` | walk + `name_to_handle_at`, then inode order | `open_by_handle_at` | ext4's nearest to xfsdump |
| 2 | Kernel-cooperative | `bulkstat` | `XFS_IOC_BULKSTAT`, inode order, no walk | `open_by_handle_at` | xfsdump |
| 3 | Raw parse | `e2fs` | libext2fs inode scan + `dir_iterate` | own `pread` on the device | dump |
| 3 | Raw parse | `raw` | hand-written ext4 reader (superblock, group descriptors, inode tables, extent trees, directory blocks) | own `pread` on the device, file by file in inode order | |
| 3 | Raw parse | `rawsort` | the same reader as `raw` | every extent of every file sorted by disk address; physically adjacent pieces are merged into one `pread` even across files | a candidate for Diavolo's read path |

`rawsort` reads in disk order, so a fragmented file's pieces arrive out of
order. Its digest doesn't care: see "Hashing". Until 2026-09-29 the hash had to
see bytes in order, so `rawsort` held early pieces in memory, up to 1.98 GiB on
`huge-aged`; it now holds none.

`e2fs` and `raw` share the transfer code in each language. The only difference
between them is who parses the metadata: the library, or our own code.

`handle` on ext4 is an **approximation** of method 2. ext4 has no bulkstat, so
the directory walk remains. What changes is that files are opened by kernel
handle, in inode order, instead of by path in walk order. `bulkstat` is the
faithful xfsdump method, so XFS images are included to show it.

## Hashing

With `--hash` (the `*-hash` regimes and the gate), every file's digest is
**BLAKE3 of its logical bytes, holes as zeros**, so it equals `b3sum` of the
file. Each method hands the engine `(file, logical offset, bytes)` in whatever
order it reads them.

**How a digest is built:**

- Each delivery is split into aligned power-of-two subtrees of 1 KiB–1 MiB.
- Each subtree is hashed to a BLAKE3 chaining value at once, and kept as 48
  bytes.
- When a file's delivered bytes reach the bytes it has on disk, its subtrees
  are merged in tree order. Holes and uninitialised extents become zero
  subtrees.
- Arrival order doesn't matter, and nothing is ever held back. This is
  advisor report P1.1, proven in `../levelup/b3tree`.

**What each language uses:**

| | C | Rust |
|---|---|---|
| subtree CVs and merge | `../levelup/b3tree` (our tree layer, upstream asm kernels) | `blake3::hazmat` |
| sorts (`rawsort` plan, `handle` inode order, per-file subtrees) | `../levelup/sort/radix.h` | a copy of the same radix sort (`sort_unstable_by_key` for per-file subtrees) |
| `--threads N` | `../levelup/pool` | `rust/src/pool.rs`, a port of it (rayon measured up to 6 % slower: see `../levelup/README.md`) |

**With `--threads N` (default 1):**

- Reads go into an arena of 2N MiB instead of the 1 MiB buffer.
- When the arena fills, all its subtrees are hashed in parallel, then the files
  they completed are merged.
- Reading itself stays on one thread, and each method's read pattern is
  unchanged.
- In both languages the reading thread is one of the N threads.

Hashing time counts as `read_ns`, as before.

## What every run reports

- `find_ns` / `read_ns`: `read` covers the content-read syscalls, plus hashing
  when hashing is on. `find` is everything else: walking, metadata, opening,
  extent mapping and directory parsing. The methods differ mostly in *find*.
- `utime`/`stime`, page faults, context switches and peak RSS (`getrusage`).
- In the harness, also:
  - loop-device reads and sectors, giving the mean request size the device saw;
  - `perf` user/kernel cycles and instructions, pinned to one CPU.

## The correctness gate

Before anything is timed, every method × implementation on every image writes
a digest file: `inode size blake3` per regular file (64 hex digits), plus
counts of files, names and bytes. All of them must be **byte-identical**, and must have found at least
one file. An image that fails is excluded from timing.

This caught three real problems during development:

- **raw parse found a file VFS couldn't see.** It was ext4's hidden `orphan_file`
  (inode 12, created by e2fsprogs 1.47). The raw readers now skip every inode
  the superblock claims as internal.
- **A content comparison passed on two empty trees.** That's why the gate now
  requires `files > 0`.
- **The text and random variants of `mixed` had different tree shapes.** They
  are now seeded independently, so content is the only thing that differs.

## File sets (`gen/mkset.py`, deterministic)

| Set | Contents (at SCALE=1) |
|---|---|
| `huge` | one 2 GiB file |
| `tiny` | 100,000 files of 0–4 KiB in 256 dirs |
| `small` | 30,000 files of 4–64 KiB in 256 dirs |
| `mixed` | ~1.5 GiB: mostly small, some medium, a few large files in a random tree; 1% sparse, 2% hardlinked, 1% symlinks |
| `real` | a copy of `REAL_SRC` (default `/usr/share`: 2.6 GiB, 111k files) |

**Layout:** `fresh` writes every file whole, in creation order, so inode order
equals disk order. `aged`:
- creates every inode first;
- writes the data in shuffled order, interleaving large files and syncing them
  chunk by chunk;
- writes 30% filler files alongside the real ones, then deletes them;
- rewrites 20% of the files into the holes the fillers leave.

The result is fragmented large files, and an inode order that no longer
matches disk order. Without this, any benefit from ordering would stay invisible.

**Content:** `random` or `text`. None of these readers compress, so content
*should not* matter. The `mixed-aged-text` set is there as a **negative control**:
if it differs from `mixed-aged-random`, something other than the method is
being measured.

Each image's `.info` records how the set actually landed (`gen/layout.py`, via
FIEMAP). It gives extents per file, and **backward**: the share of steps, visiting
files in inode order, where the disk address goes down. Fresh sets measure
around 0–2% and aged ones around 50%. That figure, not e2fsck's non-contiguous
percentage, is what inode-order reading cares about. For single-extent files,
e2fsck reads 0% whether the set is fresh or aged. ext4's per-inode preallocation
resists fragmenting one huge file, so check `huge-aged`'s extent count rather
than assuming it.

The generator produces the same logical tree on any filesystem. So a set's ext4
and XFS images hold identical bytes, and differ only in how each allocator
placed them.

## Regimes

| Regime | What it isolates |
|---|---|
| `cold` | `drop_caches` before every run: the device and the method. Expect languages to tie here |
| `warm` | everything already cached: syscall and CPU cost of each method |
| `warm-hash` | as warm, plus hashing every byte: CPU-bound, where compiler and language codegen show up |

Within each repetition, every implementation × method pair runs once, in a
freshly shuffled order, so slow drift can't systematically favour one of them.

## Running it

```sh
make -C bench/access                          # as yourself (cargo uses your ~/.cargo)
doas env SCALE=0.02 REPS=1 REAL_SRC=/usr/share/terminfo bench/access/run.sh all   # smoke test
                                              # (SCALE does not shrink the real set; REAL_SRC does)
doas bench/access/run.sh all                  # the real run
python3 bench/access/report.py                # as yourself -> results/latest/report.md
```

Knobs (environment variables): `W` (work dir, default `/var/tmp/accbench`),
`SCALE`, `REPS` (3), `REGIMES`, `EXT4_SETS`, `XFS_SETS`, `TRACE_SETS`,
`REAL_SRC`, `CPU` (2), `THREADS` (1: `--threads` for `verify` and the
`*-hash` regimes), `CPUS` (all: where runs with `THREADS` > 1 are pinned),
`FORCE=1` (rebuild images), `METHODS` (for example
`"vfs raw rawsort"`, to time only those). Each image records the `SCALE`
and `REAL_SRC` it was built with, on the first line of its `.info` file. An image
built with other values is rebuilt by `prepare` and never verified or timed. So a
smoke run's small images can't end up inside a full run's results. The steps are `prepare`,
`verify`, `bench` and `trace`, and `all` runs all four; `clean` deletes the
images but keeps the results.

The C build also reads `ACCBENCH_PLAN_DUMP=FILE`: `rawsort` then writes its
unsorted read plan (`piece_t[]`, 24 bytes per piece) to `FILE` and exits without
reading any data. `../levelup/sort` uses the dumps. The images are world-readable,
so this runs as yourself: `ACCBENCH_PLAN_DUMP=x.plan c/build/accbench-gcc rawsort IMAGE`.

## Know before you trust the numbers

- **Only the one machine is measured.** Each run records its environment in
  `env.txt`: the kernel, the command line (currently `mitigations=off`, which
  cuts syscall cost), the CPU governor and the compiler versions.
- **The images live on the SSD.** Ordering gains that a spinning disk would show
  are mostly hidden. Set `W` to a directory on an ext4 or XFS filesystem on a
  hard disk to see them. `/mnt/aladdin` is NTFS and would add its own
  fragmentation layer.
- **Loop devices use direct I/O.** This stops the backing file's page cache from
  serving reads a second time. Cold means cold for every method.
- **`drop_caches` empties your desktop's page cache too**, before every cold run.
- **Space:** the full default matrix makes about 50 GB of sparse image files,
  roughly 25 GB of it actually written. Runtime is on the order of 1–2 hours,
  most of it building images.
- **What's deliberately not reproduced:**
  - dump's own 4 KiB-per-`read` pattern: both raw readers merge contiguous
    extents into reads of up to 1 MiB;
  - libext2fs's default 8-block inode-scan buffer: both raw readers scan 1 MiB
    at a time, so `e2fs` versus `raw` compares parsers, not buffer sizes.
- **Not covered:**
  - io_uring and multithreaded reading: that's the concurrency axis, not
    access (`--threads` parallelises only the hashing);
  - O_DIRECT: that's the cache-path axis;
  - `raw` does not handle block-mapped files, inline data or meta_bg. The
    images are made without them, and `raw` refuses rather than misreads.
- **The content hash is BLAKE3 since 2026-09-29.** Before that it was a fast,
  non-cryptographic 64-bit fingerprint, so `*-hash` timings from earlier runs
  aren't comparable with later ones. Verify stamps now carry `hash=blake3`, and
  `bench` skips images whose stamp lacks it, so run `verify` (or `all`) once.

## Layout

```
c/accbench.c          C implementation (make builds accbench-gcc, accbench-clang; accbench-tsan on request)
../levelup/           b3tree, radix.h and pool, compiled into the C build
rust/src/*.rs         Rust implementation (standalone Cargo package, own [workspace])
rust/src/sys/*.rs     bindgen output for libext2fs and XFS; regenerate with rust/gen-bindings.sh
gen/mkset.py          file-set generator
gen/layout.py         FIEMAP layout measure (fragmentation, inode-order disorder)
run.sh                root harness
report.py             summary tables
```
