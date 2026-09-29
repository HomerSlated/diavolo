# bench/levelup — can C match what Rust gave for free?

On the access benchmark Rust tied C on wall time
([`../access`](../access/README.md)), but three things came built in on the
Rust side:

- BLAKE3 subtree hashing (`blake3::hazmat`);
- a fast sort (`sort_unstable`);
- a parallel runtime (rayon).

Meanwhile Rust's binaries were 10× larger. The decision rule for choosing a
language: **if speed is roughly equal, binary size decides.** These are three
viability studies, one for each of those Rust advantages. Each one gives Rust
the same chance to improve.

Measured 2026-09-29 on the Ryzen 5 4500 (Zen 2, AVX2, no AVX-512) with gcc
14.2, clang 21.1 and rustc 1.98. All buffers are in memory and touched before
timing starts. Runs are pinned with `taskset`. Figures are medians.

## Results at a glance

| Rust advantage | C answer | Speed | Size |
|---|---|---|---|
| `blake3::hazmat` subtree CVs | [`b3tree/`](b3tree): our tree layer over upstream's kernels | C 3.73–3.76, Rust 3.71–3.81 GiB/s (1 thread): **level** | BLAKE3 adds about the same to either binary (≈ 75–85 KB) |
| `sort_unstable_by_key` | [`sort/`](sort): our LSD radix sort (≈ 60 lines) | C radix is 2–5× faster than Rust's `sort_unstable` on the aged plans. The same radix in Rust is **level** with C. Rust's stable sort wins on nearly-sorted plans | negligible |
| rayon | [`pool/`](pool): pthreads pool + atomic index (112 lines) | 22.5–22.8 vs 22.6 GiB/s (12 threads): **level** | C pool ≈ 1 KB; rayon ≈ 115 KB |

**All three are viable, but none of them decides the question on its own.**
With these three pieces, C matches Rust's speed. The size difference is mostly
**a fixed floor, not a multiplier:**

| stripped, x86-64 | C | Rust (`opt-level="z"`, LTO, `panic=abort`) |
|---|---|---|
| trivial tool (read a file, print its size) | 14.5 KB | 308 KB |
| + BLAKE3, runtime kernel dispatch (`b3min`) | 88 KB (`disp`) | 394 KB |
| + BLAKE3, AVX2 + SSE4.1 kernels only | 47 KB (`asm`) | 394 KB (`lean` features: no change) |
| + BLAKE3, portable C only | 27 KB (`port`) | — |
| BLAKE3 + parallel pool (`parhash`) | 52 KB (`asm`) | 480 KB |

- **The floor.** Rust's floor is its std: formatting, panics, I/O and
  backtrace support, about 290 KB more than C. C links glibc dynamically,
  while Rust links its std statically and libc dynamically. On the platforms
  where size matters most (the Amiga), C's libc is the platform's own.
- **What each piece adds.** BLAKE3 adds a similar amount in both languages.
  A parallel runtime adds a lot to Rust (rayon) and almost nothing to C.
- **Like-for-like kernels.** Rust's `blake3` picks its kernels at run time. So
  C's like-for-like comparison is `disp`: all four x86-64 kernels, chosen at
  start-up, in a baseline-x86-64 build. That's 88 KB against 394 KB.

## 1. `b3tree/` — BLAKE3 subtree chaining values in C

**The tree layer is our own code** (`b3tree.c`; the upstream kernels are
vendored):

- `b3_subtree(in, len, off)` gives the non-root CV of any aligned
  power-of-two piece from 1 KiB to 1 MiB, or of a file's short last piece.
- `b3_parent` / `b3_root_parent` merge two subtrees.
- `b3_root_of_pieces` merges CVs of 1 MiB pieces held in file order.
- `b3_hash` hashes a whole input as the root.

A piece's CV depends only on its bytes and its offset. So a disk-order reader
can hash pieces as they arrive and keep 32 bytes per MiB instead of stashing
data. (This is report P1.1, which cut `rawsort`'s memory use from 1.98 GiB to
kilobytes.)

**How it hashes.** Upstream's hasher walks the tree depth-first. Ours works
breadth-first:

- all full chunks go through one `hash_many` call;
- each layer of parents goes through another, alternating between two CV
  arrays.

Pairing neighbours, and carrying an odd one up to the next layer, builds
exactly BLAKE3's left-complete tree. The width is `B3_WIDE` chunks:

- **1024 (1 MiB) for SIMD kernels**, about 56 KiB of stack.
- **16 for the portable kernel**, under 1 KiB. The portable kernel gains
  nothing from width, and Amiga task stacks are small.

Wider inputs split at `b3_left_len()`, so every width builds the same tree.
Widths 2, 64 and 1024 were proven for `asm` and `port`.

**The kernels are upstream BLAKE3 1.8.7's,** vendored from the crate's `c/`
directory (CC0 / Apache-2.0; licences in `vendor/`). They're renamed `b3v_*`
so that the proof's reference, the system `libblake3`, shares no code with us.

| flavour | kernels | gcc | clang |
|---|---|---|---|
| `asm` | x86-64 assembly (AVX2 + SSE4.1), what Rust's crate links, `-march=native` | 3.73 GiB/s | 3.76 GiB/s |
| `disp` | all four assembly sets (AVX-512, AVX2, SSE4.1, SSE2), picked at start-up; baseline x86-64 build | 3.75 GiB/s | — |
| `intr` | upstream's C intrinsics | 2.50 GiB/s | 3.45 GiB/s |
| `port` | portable C only: the non-x86 path | 0.72 GiB/s | 0.76 GiB/s |

- **`disp` forced down** with `B3_FORCE=sse41` runs at 2.15 GiB/s, and with
  `B3_FORCE=sse2` at 1.81 GiB/s.
- **On the same machine:**
  - Rust `blake3` 1.8.7 with hazmat: 3.71–3.78 GiB/s;
  - `blake3::hash`: 3.72–3.81 GiB/s;
  - the system `libblake3` hasher: 3.73–3.74 GiB/s.

**Correctness.** `b3prove` is a C port of the advisor's Rust proof (report
Appendix B). It passes for every flavour under both gcc and clang, and for
`disp` forced to SSE4.1 and to SSE2:

- 224 boundary cases (lengths 0 to 100 MiB, random 4 KiB-aligned extents,
  shuffled arrival, with and without holes) all match `libblake3`;
- the 160 MiB whole-file roots match `b3sum`;
- the holes case matches the Rust proof's root, which confirms the port's
  random-number sequence is exact.

`b3min`, a minimal one-file `b3sum`, matches `b3sum` on four files: 160 MiB,
1,000,000 bytes, exactly 3 MiB, and empty.

**Untested:**

- the AVX-512 path (this CPU doesn't have it);
- big-endian `port`. `store_cv` is endian-neutral, but only little-endian
  hosts have run it.

## 2. `sort/` — the read plan's sort

**Input.** `rawsort`'s real plans, dumped from every ext4 image with
`ACCBENCH_PLAN_DUMP` (see [`../access`](../access/README.md#running-it)).

**Shape of the data:**

- 2,000–112,000 pieces per plan;
- keys fit in 18–21 bits here, but a multi-TB disk needs 30 or more;
- pblk never repeats;
- aged plans are random (50 % ascending pairs);
- fresh plans are nearly sorted (12–3,500 runs).

Every candidate must reproduce glibc `qsort`'s output byte for byte.

The figures below are the range of medians over three back-to-back rounds of
every harness (µs, gcc). 
| plan (pieces) | glibc `qsort` | C radix | Rust `sort_unstable` | Rust `sort` (stable) | Rust radix |
|---|---|---|---|---|---|
| tiny-aged (99,972) | 12,129–12,162 | 982–1,503 | 2,459–2,536 | 3,749–3,785 | 933–972 |
| tiny-fresh (99,972) | 4,103–4,421 | 1,049–1,118 | 1,982–1,994 | **810–837** | 1,012–1,090 |
| real-aged (112,463) | 13,848–13,860 | 1,200–1,230 | 2,806–2,808 | 4,329–4,405 | 1,248–1,383 |
| real-fresh (112,082) | 6,077–6,172 | 1,246–1,332 | 2,383–2,404 | 3,163–3,194 | 1,197–1,228 |
| small-aged (30,000) | 3,205–3,210 | 133–143 | 664–667 | 904–927 | 151–164 |
| small-fresh (30,000) | 1,068–1,103 | 153–161 | 533–536 | 555–567 | 146–197 |

**Reading the table:**

- **C versus glibc.** C radix is 4–24× faster than glibc `qsort` (least on
  the nearly-sorted plans), and 1.8–5× faster than Rust's `sort_unstable`
  (what accbench's Rust uses).
- **Radix in both languages.** The same radix in Rust is level with C, within
  run-to-run noise. The 1,503 µs is one outlier round.
- **Nearly sorted plans.** Rust's stable `sort` (driftsort, which merges
  natural runs) beats both radix sorts on tiny-fresh, which has 12 runs. The
  radix sort only notices input that is fully sorted.
  - C could add run detection: count descents in the sortedness scan, then
    merge a few runs.
  - That's about 0.25 ms on one plan, so it's left undone.
- **Our introsort** (C, comparator inlined) came in at 5.4 ms on tiny-aged, 2×
  slower than Rust's ipnsort, so it was dropped.

**The radix sort is `s_radix`:**

- it takes the pass count and digit width from the OR of all keys, with digits
  of at most 11 bits, so two passes here;
- one scan builds every pass's histogram;
- passes where all keys share a digit are skipped;
- a sorted input is detected in O(n) and returned at once.

**What didn't work:**

- **8-bit digits** were about 2× slower: three passes instead of two. Each
  pass costs about 5 ns per element, scattering 24-byte records through L3.
- **Sorting packed (pblk << 24 | index) words and gathering afterwards** was
  slower than moving the records.

**Rust's scatter doesn't need `unsafe`.** Checked and unchecked versions time
the same.

**Scale check.** The whole gap is milliseconds against runs of 1.3 s or more.
The point is that C no longer loses here, and does so in about 60 lines. It is
not a wall-clock win.

## 3. `pool/` — a C equivalent of rayon, for Diavolo's shape of work

**What it is.** `pool.c` is a persistent pthreads pool with a single
`pool_for(n, fn, ctx)`:

- workers claim indices with a relaxed atomic fetch-add;
- the caller works too;
- one mutex and condvar pair publishes each job and collects the workers.

Hashing 1 MiB pieces has no nesting, so there's nothing to steal, and a
Chase-Lev deque would add code without adding speed. ThreadSanitizer reports
nothing (`build/parhash-tsan`).

**Throughput, 1 GiB in memory, GiB/s:**

| threads | 1 | 2 | 4 | 6 | 8 | 12 |
|---|---|---|---|---|---|---|
| C pool + b3tree | 3.76 | 7.51 | 14.69 | 16.0–20.1* | 21.00 | 22.46–22.78 |
| rayon `par_iter_mut` + hazmat | 3.74 | 7.45 | 14.42 | 18.05 | 20.93 | 22.55 |
| `blake3::Hasher::update_rayon` | 3.63 | 7.29 | 14.21 | 17.72 | 20.98 | 22.08 |

\* **The 6-thread figure swings between runs.** It's likely that threads
sometimes share SMT siblings (two logical CPUs on one physical core); that
isn't verified.

**Scaling.** Above 8 threads everything flattens at about 22 GiB/s. That's
most likely memory bandwidth.

**Sizes.**

- The Rust `parhash` also contains `update_rayon`, which has no C
  counterpart.
- Rayon's cost is therefore estimated from `parhash` (562 KB release) minus
  `b3min` (447 KB release), which is about 115 KB.

## Build and run

```sh
make -C bench/levelup/b3tree        # b3prove/b3speed/b3min x {asm,intr,port,disp}; CC=clang to switch
make -C bench/levelup/sort          # needs plans: see above
make -C bench/levelup/pool
build/b3prove-asm FILE              # then compare with: b3sum FILE
B3_FORCE=sse2 build/b3prove-disp FILE
taskset -c 2 build/b3speed-asm 1024 7
taskset -c 2 build/sortbench-gcc PLANS...
taskset -c 0-11 build/parhash 1024 7
```

Each directory's `rust/` is a standalone Cargo package with the Rust peer.
Build it with `cargo build --release --offline`, and use `--profile small` for
Rust's smallest build. `b3tree/rust` also has `--features lean` (no AVX-512 or
SSE2 kernels).

## Not done here (needs a go-ahead)

- **Putting `b3tree`, radix and pool into `accbench`,** in both languages.
  That's P1.1, P2.2 and P2.4 of the advisor's report. Today accbench's content
  hash is still the 64-bit fingerprint.
- **Run detection in the C radix sort,** for nearly-sorted plans.
- **A NEON build for aarch64.**
- **An m68k cross-build of `port`,** to see the Amiga size and stack use for
  real.
