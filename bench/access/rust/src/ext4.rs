//! Method 3: raw parse. The extent lists and data transfer shared by both
//! raw readers, and method 3b, the hand-written ext4 reader.

use std::ffi::{CString, c_int};
use std::time::Instant;

use crate::common::{BUFSZ, Ctx, Hasher, die, die_os, errno};

pub struct Ext {
    pub lblk: u64,
    pub pblk: u64,
    pub len: u32,
    pub uninit: bool,
}

pub struct Fent {
    pub ino: u64,
    pub size: u64,
    pub first: usize, // range in Parsed::ex
    pub n: usize,
}

#[derive(Default)]
pub struct Parsed {
    pub ex: Vec<Ext>,
    pub files: Vec<Fent>,
    pub dirs: Vec<Fent>,
    /// Regular inodes the filesystem keeps for itself and no directory names:
    /// journal, quota files, snapshot, and the orphan file (e2fsprogs >= 1.47
    /// creates one by default). A raw scan sees them; the VFS never does.
    pub internal: [u32; 6],
}

impl Parsed {
    pub fn is_internal(&self, ino: u64) -> bool {
        self.internal.iter().any(|&i| i != 0 && i as u64 == ino)
    }
}

pub fn pread_full(fd: c_int, mut buf: &mut [u8], mut off: u64) {
    while !buf.is_empty() {
        let r = unsafe { libc::pread(fd, buf.as_mut_ptr().cast(), buf.len(), off as libc::off_t) };
        if r < 0 {
            if errno() == libc::EINTR {
                continue;
            }
            die_os(format_args!("pread at {off}"));
        }
        if r == 0 {
            die(format_args!("unexpected end of device at {off}"));
        }
        buf = &mut buf[r as usize..];
        off += r as u64;
    }
}

/// Read one file's extents in logical order, merging runs that are contiguous on disk.
fn xfer_file(ctx: &mut Ctx, fd: c_int, bs: u64, f: &Fent, ex: &[Ext]) {
    let (mut pos, size) = (0u64, f.size);
    let mut hs = Hasher::new();
    let mut i = 0;
    while i < ex.len() {
        let lstart = ex[i].lblk * bs;
        if lstart >= size {
            break;
        }
        if lstart > pos {
            // hole: never read, only hashed as zeros
            if ctx.hash {
                let t0 = Instant::now();
                hs.zeros(lstart - pos);
                ctx.read_ns += t0.elapsed().as_nanos() as u64;
            }
            pos = lstart;
        }
        if ex[i].uninit {
            let end = ((ex[i].lblk + ex[i].len as u64) * bs).min(size);
            if ctx.hash {
                let t0 = Instant::now();
                hs.zeros(end - pos);
                ctx.read_ns += t0.elapsed().as_nanos() as u64;
            }
            pos = end;
            i += 1;
            continue;
        }
        let (lb, pb, mut nb) = (ex[i].lblk, ex[i].pblk, ex[i].len as u64);
        let mut j = i + 1;
        while j < ex.len() && !ex[j].uninit && ex[j].lblk == lb + nb && ex[j].pblk == pb + nb {
            nb += ex[j].len as u64;
            j += 1;
        }
        let end = ((lb + nb) * bs).min(size);
        let mut off = pb * bs + (pos - lb * bs);
        while pos < end {
            let chunk = ((end - pos) as usize).min(BUFSZ);
            let t0 = Instant::now();
            pread_full(fd, &mut ctx.buf[..chunk], off);
            if ctx.hash {
                hs.update(&ctx.buf[..chunk]);
            }
            ctx.read_ns += t0.elapsed().as_nanos() as u64;
            off += chunk as u64;
            pos += chunk as u64;
        }
        i = j;
    }
    if pos < size && ctx.hash {
        // trailing hole
        let t0 = Instant::now();
        hs.zeros(size - pos);
        ctx.read_ns += t0.elapsed().as_nanos() as u64;
    }
    let dg = if ctx.hash { hs.finish() } else { 0 };
    ctx.add_record(f.ino, size, dg);
}

pub fn xfer_all(ctx: &mut Ctx, fd: c_int, bs: u32, p: &Parsed) {
    for f in &p.files {
        xfer_file(ctx, fd, bs as u64, f, &p.ex[f.first..f.first + f.n]);
    }
}

// ---- method 3b: hand-written ext4 reader ----

#[inline]
fn le16(p: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([p[o], p[o + 1]])
}
#[inline]
fn le32(p: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(p[o..o + 4].try_into().unwrap())
}

fn walk_extents(fd: c_int, bs: u32, node: &[u8], level: u32, ex: &mut Vec<Ext>) {
    let (entries, depth) = (le16(node, 2) as usize, le16(node, 6));
    if le16(node, 0) != 0xF30A || 12 + entries * 12 > node.len() || level > 5 {
        die("bad extent node");
    }
    for i in 0..entries {
        let e = &node[12 + 12 * i..24 + 12 * i];
        if depth == 0 {
            let mut len = le16(e, 4) as u32;
            let uninit = len > 32768;
            if uninit {
                len -= 32768;
            }
            let pblk = (le16(e, 6) as u64) << 32 | le32(e, 8) as u64;
            ex.push(Ext {
                lblk: le32(e, 0) as u64,
                pblk,
                len,
                uninit,
            });
        } else {
            let mut child = vec![0u8; bs as usize];
            let leaf = (le16(e, 8) as u64) << 32 | le32(e, 4) as u64;
            pread_full(fd, &mut child, leaf * bs as u64);
            walk_extents(fd, bs, &child, level + 1, ex);
        }
    }
}

/// Passes 1 and 2 (inode tables, directories); returns the open device for the data.
fn raw_parse(ctx: &mut Ctx, dev: &str) -> (c_int, u32, Parsed) {
    let c = CString::new(dev).unwrap();
    let fd = unsafe { libc::open(c.as_ptr(), libc::O_RDONLY | libc::O_CLOEXEC) };
    if fd < 0 {
        die_os(format_args!("open {dev}"));
    }
    let mut sb = [0u8; 1024];
    pread_full(fd, &mut sb, 1024);
    if le16(&sb, 56) != 0xEF53 {
        die(format_args!("{dev}: not ext2/3/4"));
    }
    let (incompat, ro_compat) = (le32(&sb, 96), le32(&sb, 100));
    if incompat & 0x10 != 0 {
        die("meta_bg unsupported");
    }
    if incompat & 0x40 == 0 {
        die("extents feature required");
    }
    let bs = 1024u32 << le32(&sb, 24);
    let (ipg, bpg, first_data) = (le32(&sb, 40), le32(&sb, 32), le32(&sb, 20));
    let rev = le32(&sb, 76);
    let isz = if rev != 0 {
        le16(&sb, 88) as usize
    } else {
        128
    };
    let first_ino = if rev != 0 { le32(&sb, 84) } else { 11 } as u64;
    let hi = if incompat & 0x80 != 0 {
        (le32(&sb, 336) as u64) << 32
    } else {
        0
    };
    let blocks = le32(&sb, 4) as u64 | hi;
    let descsz = if incompat & 0x80 != 0 {
        le16(&sb, 254) as usize
    } else {
        32
    };
    let csum = ro_compat & (0x10 | 0x400) != 0; // GDT_CSUM or METADATA_CSUM: itable_unused is valid
    let groups = (blocks - first_data as u64).div_ceil(bpg as u64);

    let mut p = Parsed::default();
    for (i, off) in [224, 576, 580, 620, 640, 384].into_iter().enumerate() {
        p.internal[i] = le32(&sb, off);
    }

    let mut gd = vec![0u8; groups as usize * descsz];
    pread_full(fd, &mut gd, (first_data as u64 + 1) * bs as u64);
    let mut it = vec![0u8; BUFSZ];

    // pass 1: inode tables, one sequential sweep, 1 MiB at a time
    for g in 0..groups {
        let d = &gd[g as usize * descsz..];
        let itab = le32(d, 8) as u64
            | if descsz >= 64 {
                (le32(d, 0x28) as u64) << 32
            } else {
                0
            };
        let unused = le16(d, 0x1C) as u32
            | if descsz >= 64 {
                (le16(d, 0x32) as u32) << 16
            } else {
                0
            };
        let mut used = ipg;
        if csum {
            if le16(d, 0x12) & 1 != 0 {
                continue; // INODE_UNINIT
            }
            used = ipg - unused;
        }
        let (mut off, mut left, mut idx) = (itab * bs as u64, used as u64 * isz as u64, 0u64);
        while left != 0 {
            let chunk = (left as usize).min(BUFSZ);
            pread_full(fd, &mut it[..chunk], off);
            for inode in it[..chunk].chunks_exact(isz) {
                let ino = g * ipg as u64 + idx + 1;
                idx += 1;
                let typ = le16(inode, 0) & 0xF000;
                if le16(inode, 26) == 0 {
                    continue;
                }
                if typ != 0x4000 && !(typ == 0x8000 && ino >= first_ino && !p.is_internal(ino)) {
                    continue;
                }
                let fl = le32(inode, 32);
                if fl & 0x10000000 != 0 {
                    die(format_args!("ino {ino}: inline data unsupported"));
                }
                if fl & 0x80000 == 0 {
                    die(format_args!("ino {ino}: block-mapped files unsupported"));
                }
                let size = le32(inode, 4) as u64 | (le32(inode, 108) as u64) << 32;
                let first = p.ex.len();
                walk_extents(fd, bs, &inode[40..100], 0, &mut p.ex);
                let f = Fent {
                    ino,
                    size,
                    first,
                    n: p.ex.len() - first,
                };
                if typ == 0x4000 {
                    p.dirs.push(f)
                } else {
                    p.files.push(f)
                }
            }
            off += chunk as u64;
            left -= chunk as u64;
        }
    }
    drop(it);
    drop(gd);

    // pass 2: directories, linear walk of every block (htree nodes hide as empty entries)
    let bsz = bs as usize;
    for d in &p.dirs {
        for e in &p.ex[d.first..d.first + d.n] {
            if e.uninit {
                continue;
            }
            let (mut left, mut off) = (e.len as u64 * bs as u64, e.pblk * bs as u64);
            while left != 0 {
                let chunk = (left as usize).min(BUFSZ);
                pread_full(fd, &mut ctx.buf[..chunk], off);
                for blk in ctx.buf[..chunk].chunks_exact(bsz) {
                    let mut o = 0usize;
                    while o + 8 <= bsz {
                        let din = le32(blk, o);
                        let rl = le16(blk, o + 4) as usize;
                        let nl = blk[o + 6];
                        if rl < 8 || o + rl > bsz {
                            die(format_args!("dir ino {}: bad rec_len", d.ino));
                        }
                        let name = &blk[o + 8..o + 8 + (nl as usize).min(2)];
                        if din != 0 && !((nl == 1 && name == b".") || (nl == 2 && name == b"..")) {
                            ctx.names += 1;
                        }
                        o += rl;
                    }
                }
                off += chunk as u64;
                left -= chunk as u64;
            }
        }
    }

    (fd, bs, p)
}

pub fn run_raw(ctx: &mut Ctx, dev: &str) {
    let (fd, bs, p) = raw_parse(ctx, dev);
    xfer_all(ctx, fd, bs, &p); // pass 3: file data in inode order, file by file
    unsafe { libc::close(fd) };
}

// ---- method 3c: hand-written ext4 reader, every extent of every file in physical order ----

struct Piece {
    pblk: u64,
    nblk: u32, // at most BUFSZ / bs
    file: u32, // index into Parsed::files
    ext: u32,  // index into Parsed::ex
    off: u32,  // first block of this piece within its extent
}

struct Stashed {
    ext: u32, // relative to the file's first extent
    off: u32,
    data: Vec<u8>,
}

/// Per-file reassembly for hashing: the digest must see a file's bytes in
/// logical order, but physical order can deliver them out of order. In-order
/// bytes are hashed at once; early ones wait in the stash. Holes and uninit
/// extents are hashed as zeros when the cursor reaches them.
struct FState {
    hs: Hasher,
    pos: u64, // logical bytes hashed so far
    cur: u32, // extent the cursor is in, relative to the file's first
    done: bool,
    stash: Vec<Stashed>,
}

struct Sorted<'a> {
    p: &'a Parsed,
    bs: u64,
    fs: Vec<FState>,
    stash_bytes: u64,
    stash_peak: u64,
}

impl Sorted<'_> {
    fn advance(&mut self, ctx: &mut Ctx, fi: usize) {
        let f = &self.p.files[fi];
        let st = &mut self.fs[fi];
        while !st.done {
            let k = st.cur as usize;
            if k == f.n || self.p.ex[f.first + k].lblk * self.bs >= f.size {
                st.hs.zeros(f.size - st.pos); // trailing hole
                ctx.add_record(f.ino, f.size, st.hs.finish());
                st.done = true;
                break;
            }
            let e = &self.p.ex[f.first + k];
            let lstart = e.lblk * self.bs;
            let lend = ((e.lblk + e.len as u64) * self.bs).min(f.size);
            if st.pos < lstart {
                st.hs.zeros(lstart - st.pos); // hole
                st.pos = lstart;
            } else if e.uninit {
                st.hs.zeros(lend - st.pos);
                st.pos = lend;
                st.cur += 1;
            } else if st.pos >= lend {
                st.cur += 1;
            } else {
                break; // waiting for this extent's data
            }
        }
    }

    fn deliver(&mut self, ctx: &mut Ctx, pc: &Piece, data: &[u8]) {
        let fi = pc.file as usize;
        let first = self.p.files[fi].first;
        let rel = pc.ext - first as u32;
        let st = &mut self.fs[fi];
        if rel != st.cur || (self.p.ex[pc.ext as usize].lblk + pc.off as u64) * self.bs != st.pos {
            st.stash.push(Stashed {
                ext: rel,
                off: pc.off,
                data: data.to_vec(),
            });
            self.stash_bytes += data.len() as u64;
            self.stash_peak = self.stash_peak.max(self.stash_bytes);
            return;
        }
        st.hs.update(data);
        st.pos += data.len() as u64;
        self.advance(ctx, fi);
        loop {
            // drain whatever was waiting for this
            let (p, bs) = (self.p, self.bs);
            let st = &mut self.fs[fi];
            let Some(i) = st.stash.iter().position(|s| {
                s.ext == st.cur && (p.ex[first + s.ext as usize].lblk + s.off as u64) * bs == st.pos
            }) else {
                break;
            };
            let s = st.stash.swap_remove(i);
            st.hs.update(&s.data);
            st.pos += s.data.len() as u64;
            self.stash_bytes -= s.data.len() as u64;
            self.advance(ctx, fi);
        }
    }
}

pub fn run_rawsort(ctx: &mut Ctx, dev: &str) {
    let (fd, bs, p) = raw_parse(ctx, dev);
    let maxblk = (BUFSZ as u32) / bs;
    let bs64 = bs as u64;

    // plan: every initialised extent inside EOF, cut to <= 1 MiB, sorted by disk address
    let mut pc = Vec::with_capacity(p.files.len() + p.ex.len());
    for (fi, f) in p.files.iter().enumerate() {
        for k in 0..f.n {
            let e = &p.ex[f.first + k];
            if e.uninit {
                continue;
            }
            let mut off = 0;
            while off < e.len && (e.lblk + off as u64) * bs64 < f.size {
                let nblk = (e.len - off).min(maxblk);
                let (file, ext) = (fi as u32, (f.first + k) as u32);
                pc.push(Piece {
                    pblk: e.pblk + off as u64,
                    nblk,
                    file,
                    ext,
                    off,
                });
                off += maxblk;
            }
        }
    }
    pc.sort_unstable_by_key(|x| x.pblk);
    let mut so = Sorted {
        p: &p,
        bs: bs64,
        fs: Vec::new(),
        stash_bytes: 0,
        stash_peak: 0,
    };
    if ctx.hash {
        let t0 = Instant::now(); // hashing leading holes is read work, as in raw
        so.fs = (0..p.files.len())
            .map(|_| FState {
                hs: Hasher::new(),
                pos: 0,
                cur: 0,
                done: false,
                stash: Vec::new(),
            })
            .collect();
        for fi in 0..p.files.len() {
            so.advance(ctx, fi); // empty and all-hole files finish here
        }
        ctx.read_ns += t0.elapsed().as_nanos() as u64;
    }

    // read: one pread per run of physically adjacent pieces, whichever files they belong to.
    // The buffer leaves ctx for the loop so deliver() can take &mut ctx without a copy.
    let buf = std::mem::take(&mut ctx.buf);
    let mut i = 0;
    while i < pc.len() {
        let (start, mut nb) = (pc[i].pblk, pc[i].nblk as u64);
        let mut j = i + 1;
        while j < pc.len() && pc[j].pblk == start + nb && nb + pc[j].nblk as u64 <= maxblk as u64 {
            nb += pc[j].nblk as u64;
            j += 1;
        }
        let t0 = Instant::now();
        let len = (nb * bs64) as usize;
        pread_full(fd, &mut buf[..len], start * bs64);
        if ctx.hash {
            let run = &buf[..len];
            for x in &pc[i..j] {
                let f = &p.files[x.file as usize];
                let lstart = (p.ex[x.ext as usize].lblk + x.off as u64) * bs64;
                let n = (x.nblk as u64 * bs64).min(f.size - lstart) as usize; // the file may end inside this piece
                let o = ((x.pblk - start) * bs64) as usize;
                so.deliver(ctx, x, &run[o..o + n]);
            }
        }
        ctx.read_ns += t0.elapsed().as_nanos() as u64;
        i = j;
    }
    ctx.buf = buf;
    if ctx.hash {
        if let Some(fi) = so.fs.iter().position(|s| !s.done) {
            die(format_args!(
                "ino {}: data never completed",
                p.files[fi].ino
            ));
        }
        if so.stash_peak != 0 {
            eprintln!(
                "accbench: rawsort held back at most {} bytes for reordering",
                so.stash_peak
            );
        }
    } else {
        for f in &p.files {
            ctx.add_record(f.ino, f.size, 0);
        }
    }
    unsafe { libc::close(fd) };
}
