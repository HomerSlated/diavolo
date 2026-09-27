//! What every method shares: the read buffer, counters, per-file records,
//! the content hash, the inode set and the fd read loop. Mirrors the
//! corresponding sections of ../c/accbench.c.

use std::alloc::{Layout, alloc};
use std::ffi::c_int;
use std::fmt::Display;
use std::time::Instant;

pub const BUFSZ: usize = 1 << 20; // every content read is at most 1 MiB
pub const HK: u64 = 0x9E3779B97F4A7C15;

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

// ---- content hash (identical in C; not cryptographic, just a fingerprint) ----

#[inline]
fn absorb(h: u64, w: u64) -> u64 {
    (h ^ w).rotate_left(27).wrapping_mul(HK)
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

pub struct Hasher {
    h: u64,
    len: u64,
    pend: u64,
    npend: u32,
}

impl Hasher {
    pub fn new() -> Self {
        Hasher {
            h: 0x243F6A8885A308D3,
            len: 0,
            pend: 0,
            npend: 0,
        }
    }

    pub fn update(&mut self, mut p: &[u8]) {
        let mut h = self.h;
        self.len += p.len() as u64;
        while self.npend != 0 && !p.is_empty() {
            self.pend |= (p[0] as u64) << (8 * self.npend);
            p = &p[1..];
            self.npend += 1;
            if self.npend == 8 {
                h = absorb(h, self.pend);
                self.pend = 0;
                self.npend = 0;
            }
        }
        let (words, rest) = p.as_chunks::<8>();
        for w in words {
            h = absorb(h, u64::from_le_bytes(*w));
        }
        for &b in rest {
            self.pend |= (b as u64) << (8 * self.npend);
            self.npend += 1;
        }
        self.h = h;
    }

    pub fn zeros(&mut self, mut n: u64) {
        static Z: [u8; 65536] = [0; 65536];
        while n != 0 {
            let k = n.min(Z.len() as u64) as usize;
            self.update(&Z[..k]);
            n -= k as u64;
        }
    }

    pub fn finish(&self) -> u64 {
        let mut h = self.h;
        if self.npend != 0 {
            h = absorb(h, self.pend);
        }
        fmix(h ^ self.len)
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

// ---- run state ----

pub struct Rec {
    pub ino: u64,
    pub size: u64,
    pub digest: u64,
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
}

impl Ctx {
    pub fn new(hash: bool) -> Self {
        let layout = Layout::from_size_align(BUFSZ, 4096).unwrap();
        // SAFETY: non-zero size; the buffer lives for the whole process.
        let buf = unsafe {
            let p = alloc(layout);
            if p.is_null() {
                die("out of memory");
            }
            p.write_bytes(0, BUFSZ); // fault the buffer in before timing
            std::slice::from_raw_parts_mut(p, BUFSZ)
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
        }
    }

    pub fn add_record(&mut self, ino: u64, size: u64, digest: u64) {
        self.recs.push(Rec { ino, size, digest });
        self.files += 1;
        self.bytes += size;
        if self.hash {
            self.combined = self
                .combined
                .wrapping_add(fmix(digest ^ ino.wrapping_mul(HK)));
        }
    }

    /// The shared read loop for fd-based methods (1 and 2). Returns (bytes, digest).
    pub fn read_stream(&mut self, fd: c_int) -> (u64, u64) {
        let mut hs = Hasher::new();
        let mut tot = 0u64;
        loop {
            let t0 = Instant::now();
            // SAFETY: buf is BUFSZ bytes and exclusively ours.
            let r = unsafe { libc::read(fd, self.buf.as_mut_ptr().cast(), BUFSZ) };
            if r < 0 {
                if errno() == libc::EINTR {
                    continue;
                }
                die_os("read");
            }
            if self.hash && r > 0 {
                hs.update(&self.buf[..r as usize]);
            }
            self.read_ns += t0.elapsed().as_nanos() as u64;
            if r == 0 {
                break;
            }
            tot += r as u64;
        }
        (tot, if self.hash { hs.finish() } else { 0 })
    }
}

pub fn dot_or_dotdot(n: &[u8]) -> bool {
    n == b"." || n == b".."
}
