//! Rust peer of ../parhash.c: multi-threaded BLAKE3 of an in-memory buffer,
//!   pieces  1 MiB pieces' CVs via rayon par_iter_mut (hazmat), then the root
//!   rayon   blake3's own Hasher::update_rayon (rayon::join down the tree)
//! each in a rayon pool of the given size, median of REPS; roots must match.
//! Usage: parhash [MiB [REPS [THREADS...]]]
use blake3::hazmat::{ChainingValue, HasherExt, Mode, merge_subtrees_non_root, merge_subtrees_root};
use blake3::{Hash, Hasher};
use rayon::prelude::*;
use std::time::Instant;

const MIB: usize = 1 << 20;

fn merge(cv: &[ChainingValue]) -> ChainingValue {
    if cv.len() == 1 {
        return cv[0];
    }
    let k = 1 << (usize::BITS - 1 - (cv.len() - 1).leading_zeros());
    merge_subtrees_non_root(&merge(&cv[..k]), &merge(&cv[k..]), Mode::Hash)
}

fn root_of_pieces(cv: &[ChainingValue]) -> Hash {
    let k = 1 << (usize::BITS - 1 - (cv.len() - 1).leading_zeros());
    merge_subtrees_root(&merge(&cv[..k]), &merge(&cv[k..]), Mode::Hash)
}

fn median(mut t: Vec<f64>) -> f64 {
    t.sort_by(f64::total_cmp);
    t[t.len() / 2]
}

fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    let mib: usize = a.first().map_or(1024, |s| s.parse().unwrap());
    let reps: usize = a.get(1).map_or(7, |s| s.parse().unwrap());
    let threads: Vec<usize> = if a.len() > 2 {
        a[2..].iter().map(|s| s.parse().unwrap()).collect()
    } else {
        vec![1, 2, 4, 6, 8, 12]
    };
    let len = mib * MIB - 777;
    let np = len.div_ceil(MIB);
    let mut x: u64 = 0x9E3779B97F4A7C15;
    let buf: Vec<u8> = (0..len)
        .map(|_| {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            (x >> 32) as u8
        })
        .collect();
    let want = blake3::hash(&buf);
    let mut cv = vec![[0u8; 32]; np];
    println!("rust rayon, blake3 1.8.7, {mib} MiB, {reps} reps, median: pieces | update_rayon");
    for &t in &threads {
        let pool = rayon::ThreadPoolBuilder::new().num_threads(t).build().unwrap();
        let (mut tp, mut tr) = (Vec::new(), Vec::new());
        for _ in 0..reps {
            let t0 = Instant::now();
            let got = pool.install(|| {
                cv.par_iter_mut().enumerate().for_each(|(i, c)| {
                    let off = i * MIB;
                    let end = (off + MIB).min(len);
                    *c = Hasher::new().set_input_offset(off as u64).update(&buf[off..end]).finalize_non_root();
                });
                root_of_pieces(&cv)
            });
            let t1 = Instant::now();
            let got2 = pool.install(|| Hasher::new().update_rayon(&buf).finalize());
            let t2 = Instant::now();
            assert!(got == want && got2 == want, "wrong root with {t} threads");
            tp.push((t1 - t0).as_secs_f64());
            tr.push((t2 - t1).as_secs_f64());
        }
        let (mp, mr) = (median(tp), median(tr));
        let g = |s: f64| len as f64 / s / (1u64 << 30) as f64;
        println!("  {t:2} threads  {mp:6.4} s  {:6.2} GiB/s | {mr:6.4} s  {:6.2} GiB/s", g(mp), g(mr));
    }
}
