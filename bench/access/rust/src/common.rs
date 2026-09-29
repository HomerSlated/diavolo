//! What every method shares: the read buffer, counters, per-file records,
//! the content hash, the radix sort, the inode set and the fd read loop.
//! Mirrors the corresponding sections of ../c/accbench.c.

use std::alloc::{Layout, alloc};
use std::ffi::c_int;
use std::fmt::Display;
use std::time::Instant;

use crate::pool::Pool;
use blake3::hazmat::{
    ChainingValue, HasherExt, Mode, left_subtree_len, merge_subtrees_non_root, merge_subtrees_root,
};

pub const BUFSZ: usize = 1 << 20; // every content read is at most 1 MiB
pub const HK: u64 = 0x9E3779B97F4A7C15;
const CHUNK: u64 = blake3::CHUNK_LEN as u64;

pub fn die(msg: impl Display) -> ! {
    eprintln!("accbench: {msg}");
    std::process::exit(2)
}

pub fn die_os(msg: impl Display) -> ! {
    let e = std::io::Error::last_os_error();
    eprintln!("accbench: {msg}: {e}");
    std::process::exit(2)
}

pub fn errno() -> c_int {
    std::io::Error::last_os_error().raw_os_error().unwrap_or(0)
}

pub fn clear_errno() {
    // SAFETY: __errno_location returns this thread's errno slot.
    unsafe { *libc::__errno_location() = 0 };
}

#[inline]
pub fn fmix(mut h: u64) -> u64 {
    h ^= h >> 33;
    h = h.wrapping_mul(0xff51afd7ed558ccd);
    h ^= h >> 33;
    h = h.wrapping_mul(0xc4ceb9fe1a85ec53);
    h ^= h >> 33;
    h
}

// ---- radix sort: a copy of ../../levelup/sort/radix.h (see the measurements there) ----

/// LSD radix sort on a 64-bit key; stable. Returns at once if already sorted,
/// insertion-sorts below 64 elements, takes digits of at most 11 bits from
/// the OR of all keys, builds every pass's histogram in one scan and skips
/// passes where all keys share a digit.
pub fn radix_sort<T: Copy>(a: &mut [T], key: impl Fn(&T) -> u64) {
    let n = a.len();
    let Some(mut i) = (1..n).find(|&i| key(&a[i - 1]) > key(&a[i])) else {
        return;
    };
    if n < 64 {
        while i < n {
            let x = a[i];
            let mut j = i;
            while j > 0 && key(&a[j - 1]) > key(&x) {
                a[j] = a[j - 1];
                j -= 1;
            }
            a[j] = x;
            i += 1;
        }
        return;
    }
    let all = a.iter().fold(0, |m, x| m | key(x));
    let bits = 64 - (all | 1).leading_zeros() as usize;
    let passes = bits.div_ceil(11);
    let w = bits.div_ceil(passes);
    let mask = (1u64 << w) - 1;
    let mut h = vec![[0u32; 2048]; passes];
    for x in a.iter() {
        let k = key(x);
        for (p, hp) in h.iter_mut().enumerate() {
            hp[((k >> (p * w)) & mask) as usize] += 1;
        }
    }
    let mut tmp = a.to_vec();
    let (mut src, mut dst): (&mut [T], &mut [T]) = (a, &mut tmp);
    let mut swapped = false;
    for (p, c) in h.iter_mut().enumerate() {
        if c[((key(&src[0]) >> (p * w)) & mask) as usize] as usize == n {
            continue; // every key has the same digit here
        }
        let mut sum = 0;
        for d in c.iter_mut().take(mask as usize + 1) {
            let k = *d;
            *d = sum;
            sum += k;
        }
        for x in src.iter() {
            let d = &mut c[((key(x) >> (p * w)) & mask) as usize];
            dst[*d as usize] = *x;
            *d += 1;
        }
        std::mem::swap(&mut src, &mut dst);
        swapped = !swapped;
    }
    if swapped {
        dst.copy_from_slice(src); // dst is the caller's slice again
    }
}

// ---- content hash: BLAKE3 of each file's logical bytes, holes as zeros ----
//
// Every method delivers a file's bytes as (logical offset, data), in whatever
// order it reads them. Each delivery is split into aligned power-of-two
// subtrees of 1 KiB..1 MiB, hashed to chaining values on the spot
// (blake3::hazmat) and kept as 48 bytes each. When a file's delivered bytes
// reach the bytes it has on disk, its subtrees are merged in tree order, with
// holes and uninitialised extents hashed as zero subtrees. The digest equals
// b3sum of the file, whatever the arrival order, and no data is held back.
//
// With --threads N > 1, reads land in an arena of 2N MiB instead of buf; when
// it fills, the pool hashes all its subtrees at once, then the files they
// completed are merged. The pool is a port of C's (pool.rs), and as in C the
// reading thread is one of its N threads.

const ZLEN: usize = 1 << 20;

/// 1 MiB of zeros, never written. An immutable static of zeros lands in
/// .rodata, as 1 MiB of file; interior mutability puts it in .bss, as C's is.
struct Zeros(std::cell::UnsafeCell<[u8; ZLEN]>);
// SAFETY: nothing ever writes it.
unsafe impl Sync for Zeros {}
static ZEROS: Zeros = Zeros(std::cell::UnsafeCell::new([0; ZLEN]));

fn zeros(n: usize) -> &'static [u8] {
    // SAFETY: never written, so shared references are sound.
    unsafe { &(&*ZEROS.0.get())[..n] }
}

#[derive(Clone, Copy)]
struct Run {
    off: u64,
    len: u64,
    cv: ChainingValue,
}

struct HFile {
    ino: u64,
    size: u64,
    need: u64, // bytes to be delivered (size less holes)
    got: u64,
    runs: Vec<Run>,
    whole: bool, // runs[0] is the entire file, so its cv is the root
    done: bool,
}

struct Job {
    at: usize, // arena offset of the run's data
    f: u32,
    i: u32,
}

fn subtree(data: &[u8], off: u64) -> ChainingValue {
    blake3::Hasher::new()
        .set_input_offset(off)
        .update(data)
        .finalize_non_root()
}

fn zero_cv(off: u64, len: u64) -> ChainingValue {
    if len > ZLEN as u64 {
        // a long hole: its own subtrees, merged
        let ll = left_subtree_len(len);
        merge_subtrees_non_root(&zero_cv(off, ll), &zero_cv(off + ll, len - ll), Mode::Hash)
    } else {
        subtree(zeros(len as usize), off)
    }
}

fn node(f: &HFile, off: u64, len: u64) -> ChainingValue {
    let r = &f.runs;
    let i = r.partition_point(|x| x.off < off);
    if i < r.len() && r[i].off == off && r[i].len == len {
        return r[i].cv;
    }
    let j = r.partition_point(|x| x.off < off + len); // any run inside [off, off+len)?
    if !(j > 0 && r[j - 1].off + r[j - 1].len > off) {
        return zero_cv(off, len); // a hole
    }
    if len <= CHUNK {
        die(format_args!("ino {}: subtree misaligned at {off}", f.ino));
    }
    let ll = left_subtree_len(len);
    merge_subtrees_non_root(&node(f, off, ll), &node(f, off + ll, len - ll), Mode::Hash)
}

// ---- run state ----

/// A zeroed, page-aligned buffer for the whole process, faulted in before timing.
fn page_aligned(n: usize) -> &'static mut [u8] {
    let layout = Layout::from_size_align(n, 4096).unwrap();
    // SAFETY: non-zero size; the buffer is never freed.
    unsafe {
        let p = alloc(layout);
        if p.is_null() {
            die("out of memory");
        }
        p.write_bytes(0, n);
        std::slice::from_raw_parts_mut(p, n)
    }
}

#[derive(Clone, Copy)]
pub struct Rec {
    pub ino: u64,
    pub size: u64,
    pub digest: [u8; 32],
}

pub struct Ctx {
    pub buf: &'static mut [u8],
    pub hash: bool,
    pub files: u64,
    pub names: u64,
    pub bytes: u64,
    pub read_ns: u64,
    pub combined: u64,
    pub recs: Vec<Rec>,
    pub seen: ISet,
    hf: Vec<HFile>,
    arena: &'static mut [u8], // batch mode (--threads > 1) when non-empty
    pool: Option<Pool>,
    fill: usize,
    jobs: Vec<Job>,
    done: Vec<u32>,
}

impl Ctx {
    pub fn new(hash: bool, threads: usize) -> Self {
        let buf = page_aligned(BUFSZ);
        // Page-aligned like buf: a Vec's malloc'd block starts 16 bytes into a
        // page, and every pread into it then copied ~40 % slower (measured).
        let arena = if threads > 1 && hash {
            page_aligned(2 * threads * BUFSZ)
        } else {
            &mut []
        };
        Ctx {
            buf,
            hash,
            files: 0,
            names: 0,
            bytes: 0,
            read_ns: 0,
            combined: 0,
            recs: Vec::with_capacity(4096),
            seen: ISet::default(),
            hf: Vec::new(),
            pool: (threads > 1 && hash).then(|| Pool::new(threads)), // started before timing
            arena,
            fill: 0,
            jobs: Vec::new(),
            done: Vec::new(),
        }
    }

    pub fn add_record(&mut self, ino: u64, size: u64, digest: [u8; 32]) {
        self.recs.push(Rec { ino, size, digest });
        self.files += 1;
        self.bytes += size;
        if self.hash {
            let w = u64::from_le_bytes(digest[..8].try_into().unwrap());
            self.combined = self.combined.wrapping_add(fmix(w ^ ino.wrapping_mul(HK)));
        }
    }

    fn finish(&mut self, fid: usize) {
        let f = &mut self.hf[fid];
        let d = if f.whole {
            f.runs[0].cv
        } else if f.size <= CHUNK {
            *blake3::hash(zeros(f.size as usize)).as_bytes() // empty, or all hole
        } else {
            f.runs.sort_unstable_by_key(|r| r.off);
            let ll = left_subtree_len(f.size);
            let root = merge_subtrees_root(&node(f, 0, ll), &node(f, ll, f.size - ll), Mode::Hash);
            *root.as_bytes()
        };
        let (ino, size) = (f.ino, f.size);
        f.runs = Vec::new();
        f.done = true;
        self.add_record(ino, size, d);
    }

    /// Start hashing a file of size bytes, need of them to be delivered.
    pub fn hf_open(&mut self, ino: u64, size: u64, need: u64) -> usize {
        let fid = self.hf.len();
        self.hf.push(HFile {
            ino,
            size,
            need,
            got: 0,
            runs: Vec::new(),
            whole: false,
            done: false,
        });
        if need == 0 {
            self.finish(fid); // empty or all hole: nothing will arrive
        }
        fid
    }

    fn batch(&self) -> bool {
        !self.arena.is_empty()
    }

    pub fn flush(&mut self) {
        let jobs = std::mem::take(&mut self.jobs);
        if !jobs.is_empty() {
            let (hf, arena) = (&self.hf, &self.arena);
            let mut cvs = vec![[0u8; 32]; jobs.len()];
            self.pool.as_ref().unwrap().map_into(&mut cvs, &|i| {
                let j = &jobs[i];
                let f = &hf[j.f as usize];
                let r = &f.runs[j.i as usize];
                let d = &arena[j.at..j.at + r.len as usize];
                if f.whole {
                    *blake3::hash(d).as_bytes()
                } else {
                    subtree(d, r.off)
                }
            });
            for (j, cv) in jobs.iter().zip(cvs) {
                self.hf[j.f as usize].runs[j.i as usize].cv = cv;
            }
        }
        self.jobs = jobs;
        self.jobs.clear();
        self.fill = 0;
        let done = std::mem::take(&mut self.done);
        for &fid in &done {
            self.finish(fid as usize);
        }
        self.done = done;
        self.done.clear();
    }

    /// Where to read the next want (<= BUFSZ) bytes that will be delivered, as
    /// an offset into region(); then hb_used(bytes read), once the deliveries
    /// from that read are made.
    pub fn hb_buf(&mut self, want: usize) -> usize {
        if !self.batch() {
            return 0; // buf: delivered and hashed before the next read
        }
        if self.fill + want > self.arena.len() {
            self.flush();
        }
        self.fill
    }

    pub fn region(&mut self, at: usize, len: usize) -> &mut [u8] {
        if self.batch() {
            &mut self.arena[at..at + len]
        } else {
            &mut self.buf[at..at + len]
        }
    }

    pub fn hb_used(&mut self, n: usize) {
        if self.batch() {
            self.fill += (n + 63) & !63;
        }
    }

    /// Hand the n bytes at region offset at to file fid's hash, as its bytes
    /// from logical offset off.
    pub fn deliver(&mut self, fid: usize, off: u64, at: usize, n: usize) {
        let batch = self.batch();
        let f = &mut self.hf[fid];
        let n64 = n as u64;
        if !off.is_multiple_of(CHUNK)
            || (!n64.is_multiple_of(CHUNK) && off + n64 != f.size)
            || off + n64 > f.size
        {
            die(format_args!(
                "ino {}: delivery {off}+{n} misaligned or past EOF",
                f.ino
            ));
        }
        let data: &[u8] = if batch {
            &self.arena[at..at + n]
        } else {
            &self.buf[at..at + n]
        };
        f.got += n64;
        f.whole = off == 0 && n64 == f.size;
        let (mut a, b) = (off, off + n64);
        while a < b {
            let mut sz = ZLEN as u64;
            while sz > CHUNK && (a % sz != 0 || a + sz > b) {
                sz >>= 1;
            }
            let len = if f.whole { n64 } else { sz.min(b - a) }; // the file's last chunk may be short
            let rel = (a - off) as usize;
            let cv = if batch {
                self.jobs.push(Job {
                    at: at + rel,
                    f: fid as u32,
                    i: f.runs.len() as u32,
                });
                [0; 32]
            } else if f.whole {
                *blake3::hash(data).as_bytes()
            } else {
                subtree(&data[rel..rel + len as usize], a)
            };
            f.runs.push(Run { off: a, len, cv });
            a += len;
        }
        if f.got == f.need {
            if batch {
                self.done.push(fid as u32);
            } else {
                self.finish(fid);
            }
        }
    }

    /// Every file opened for hashing was completed and recorded.
    pub fn check_complete(&self) {
        if let Some(f) = self.hf.iter().find(|f| !f.done) {
            die(format_args!("ino {}: data never completed", f.ino));
        }
    }

    /// The shared read loop for fd-based methods (1 and 2). Returns the bytes read.
    pub fn read_stream(&mut self, fd: c_int, ino: u64, size: u64) -> u64 {
        let fid = if self.hash {
            self.hf_open(ino, size, size)
        } else {
            0
        };
        let mut tot = 0u64;
        loop {
            let t0 = Instant::now();
            let at = self.hb_buf(BUFSZ);
            let (mut got, mut r) = (0usize, 1isize);
            while got < BUFSZ {
                let b = self.region(at + got, BUFSZ - got);
                // SAFETY: b is valid for b.len() bytes and exclusively ours.
                r = unsafe { libc::read(fd, b.as_mut_ptr().cast(), b.len()) };
                if r == 0 {
                    break;
                }
                if r < 0 {
                    if errno() == libc::EINTR {
                        continue;
                    }
                    die_os("read");
                }
                got += r as usize; // fill the buffer, so every delivery is aligned
            }
            if self.hash && got > 0 {
                if tot + got as u64 > size {
                    die(format_args!("ino {ino}: grew while being read"));
                }
                self.deliver(fid, tot, at, got);
                self.hb_used(got);
            }
            self.read_ns += t0.elapsed().as_nanos() as u64;
            tot += got as u64;
            if r == 0 {
                break;
            }
        }
        tot
    }
}

// ---- inode set: open addressing, key 0 = empty (inode 0 never exists) ----

#[derive(Default)]
pub struct ISet {
    k: Vec<u64>,
    n: usize,
}

impl ISet {
    pub fn add(&mut self, ino: u64) -> bool {
        if (self.n + 1) * 2 > self.k.len() {
            let nc = if self.k.is_empty() {
                1024
            } else {
                self.k.len() * 2
            };
            let mut nk = vec![0u64; nc];
            for &x in self.k.iter().filter(|&&x| x != 0) {
                let mut j = fmix(x) as usize & (nc - 1);
                while nk[j] != 0 {
                    j = (j + 1) & (nc - 1);
                }
                nk[j] = x;
            }
            self.k = nk;
        }
        let m = self.k.len() - 1;
        let mut i = fmix(ino) as usize & m;
        while self.k[i] != 0 {
            if self.k[i] == ino {
                return false;
            }
            i = (i + 1) & m;
        }
        self.k[i] = ino;
        self.n += 1;
        true
    }
}

pub fn dot_or_dotdot(n: &[u8]) -> bool {
    n == b"." || n == b".."
}
