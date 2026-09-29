//! Proof for report P1.1: BLAKE3 of a file's logical bytes (holes as zeros) computed
//! from per-piece chaining values that arrive in ANY order, then merged in tree order.
//! Pieces are cut like a disk-order reader would cut them: arbitrary 4 KiB-aligned
//! "extents", each decomposed into maximal aligned power-of-two runs capped at 1 MiB.
use blake3::hazmat::{ChainingValue, HasherExt, Mode, left_subtree_len, merge_subtrees_non_root, merge_subtrees_root};
use blake3::{CHUNK_LEN, Hasher};
use std::collections::BTreeMap;
use std::time::Instant;

const CAP: u64 = 1 << 20;

struct Pieces { map: BTreeMap<u64, (u64, ChainingValue)>, whole: Option<blake3::Hash> } // offset -> (len, cv); whole = a piece that IS the file

fn zero_cv(off: u64, len: u64) -> ChainingValue {
    static Z: [u8; 65536] = [0; 65536];
    let mut h = Hasher::new();
    h.set_input_offset(off);
    let mut left = len;
    while left > 0 { let k = left.min(Z.len() as u64) as usize; h.update(&Z[..k]); left -= k as u64; }
    h.finalize_non_root()
}

fn covered(p: &Pieces, off: u64, len: u64) -> bool {
    // any stored piece intersecting [off, off+len)?
    p.map.range(..off + len).next_back().map_or(false, |(&o, &(l, _))| o + l > off)
}

fn node(p: &Pieces, off: u64, len: u64) -> ChainingValue {
    if let Some(&(l, cv)) = p.map.get(&off) { if l == len { return cv; } }
    if !covered(p, off, len) { return zero_cv(off, len); }          // a hole: hash zeros at this offset
    assert!(len > CHUNK_LEN as u64, "stored piece does not align with the tree at {off}+{len}");
    let l = left_subtree_len(len);
    merge_subtrees_non_root(&node(p, off, l), &node(p, off + l, len - l), Mode::Hash)
}

fn root(p: &Pieces, data_for_small: &[u8], total: u64) -> blake3::Hash {
    if let Some(h) = p.whole { return h; }             // one piece covers the whole file: it is the root
    if total <= CHUNK_LEN as u64 { return blake3::hash(data_for_small); } // empty/1-chunk file that is all hole
    let l = left_subtree_len(total);
    merge_subtrees_root(&node(p, 0, l), &node(p, l, total - l), Mode::Hash)
}

/// Binary decomposition of [a, b) into aligned power-of-two runs (>= 1 KiB, <= CAP).
fn decompose(mut a: u64, b: u64, out: &mut Vec<(u64, u64)>) {
    while a < b {
        let mut sz = CAP;
        while sz > CHUNK_LEN as u64 && (a % sz != 0 || a + sz > b) { sz /= 2; }
        let len = sz.min(b - a);                      // the file's last chunk may be short
        out.push((a, len));
        a += len;
    }
}

struct Rng(u64);
impl Rng { fn next(&mut self) -> u64 { self.0 ^= self.0 << 13; self.0 ^= self.0 >> 7; self.0 ^= self.0 << 17; self.0 } }

fn run(data: &mut [u8], total: u64, seed: u64, holes: bool, timing: bool) {
    let mut rng = Rng(seed | 1);
    // cut [0,total) into "extents" at random 4 KiB boundaries; mark some as holes
    let mut cuts = vec![0u64];
    while *cuts.last().unwrap() < total {
        let step = ((rng.next() % 3000) + 1) * 4096;          // 4 KiB .. ~12 MiB
        cuts.push((cuts.last().unwrap() + step).min(total));
    }
    let mut pieces = Vec::new();
    for w in cuts.windows(2) {
        let hole = holes && rng.next() % 5 == 0;
        if hole { for x in &mut data[w[0] as usize..w[1] as usize] { *x = 0; } continue; }
        decompose(w[0], w[1], &mut pieces);
    }
    // arrival order: shuffled, as a disk-order reader of a fragmented file would see it
    for i in (1..pieces.len()).rev() { let j = (rng.next() % (i as u64 + 1)) as usize; pieces.swap(i, j); }
    let t0 = Instant::now();
    let mut p = Pieces { map: BTreeMap::new(), whole: None };
    for &(off, len) in &pieces {
        if off == 0 && len == total {   // the writer knows the file size: finalise as root at arrival
            p.whole = Some(blake3::hash(&data[..total as usize]));
            continue;
        }
        let cv = Hasher::new().set_input_offset(off).update(&data[off as usize..(off + len) as usize]).finalize_non_root();
        p.map.insert(off, (len, cv));
    }
    let t1 = Instant::now();
    let got = root(&p, &data[..total as usize], total);
    let t2 = Instant::now();
    let want = blake3::hash(&data[..total as usize]);
    let t3 = Instant::now();
    assert_eq!(got, want, "MISMATCH total={total} seed={seed} holes={holes}");
    if timing {
        println!("total={total} pieces={} (shuffled) holes={holes}: piece CVs {:.3}s, merge {:.4}s, reference blake3::hash {:.3}s, cv-map ~{} KiB  OK {}",
            pieces.len(), (t1 - t0).as_secs_f64(), (t2 - t1).as_secs_f64(), (t3 - t2).as_secs_f64(),
            p.map.len() * 48 / 1024, got.to_hex());
    }
}

fn main() {
    let path = std::env::args().nth(1).expect("file");
    let orig = std::fs::read(&path).unwrap();
    let n = orig.len() as u64;
    let mut cases = 0;
    // exhaustive-ish small and boundary lengths, several random layouts each, with and without holes
    for &total in &[0u64, 1, 1023, 1024, 1025, 4096, 12345, 65536, (1 << 20) - 1, 1 << 20, (1 << 20) + 1,
                    3 * (1 << 20) + 12345, 7 * (1 << 20) + 4096, 100 * (1 << 20) + 777] {
        if total > n { continue; }
        for seed in 1..=8u64 { for holes in [false, true] {
            let mut d = orig[..total as usize].to_vec();
            run(&mut d, total, seed * 7919 + total, holes, false); cases += 1;
        } }
    }
    println!("{cases} boundary cases OK");
    // the full file, timed; reference against b3sum printed for cross-check
    let mut d = orig.clone();
    run(&mut d, n, 42, false, true);
    let mut d = orig.clone();
    run(&mut d, n, 43, true, true);
}
