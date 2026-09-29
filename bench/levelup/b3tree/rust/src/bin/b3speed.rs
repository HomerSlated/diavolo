//! Rust peer of ../speed.c: single-thread BLAKE3 throughput on an in-memory
//! buffer, the median of REPS runs of
//!   tree  1 MiB pieces hashed in shuffled order (hazmat), then merged
//!   root  blake3::hash over the whole buffer
//! Both roots must agree. Usage: b3speed [MiB [REPS]]
use blake3::hazmat::{ChainingValue, HasherExt, Mode, merge_subtrees_non_root, merge_subtrees_root};
use blake3::{Hash, Hasher};
use std::time::Instant;

const MIB: usize = 1 << 20;

fn merge(cv: &[ChainingValue]) -> ChainingValue {
    if cv.len() == 1 {
        return cv[0];
    }
    let k = 1 << (usize::BITS - 1 - (cv.len() - 1).leading_zeros()); // largest power of two < n
    merge_subtrees_non_root(&merge(&cv[..k]), &merge(&cv[k..]), Mode::Hash)
}

fn root_of_pieces(cv: &[ChainingValue]) -> Hash {
    let k = 1 << (usize::BITS - 1 - (cv.len() - 1).leading_zeros());
    merge_subtrees_root(&merge(&cv[..k]), &merge(&cv[k..]), Mode::Hash)
}

fn main() {
    let mut a = std::env::args().skip(1);
    let mib: usize = a.next().map_or(1024, |s| s.parse().unwrap());
    let reps: usize = a.next().map_or(7, |s| s.parse().unwrap());
    let len = mib * MIB - 777; // a short last piece, as real files have
    let np = len.div_ceil(MIB);
    let mut x: u64 = 0x9E3779B97F4A7C15;
    let mut step = || {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        x
    };
    let buf: Vec<u8> = (0..len).map(|_| (step() >> 32) as u8).collect();
    let mut order: Vec<usize> = (0..np).collect();
    for i in (1..np).rev() {
        order.swap(i, (step() % (i as u64 + 1)) as usize);
    }
    let mut cv = vec![[0u8; 32]; np];
    let (mut tt, mut tr) = (Vec::new(), Vec::new());
    let (mut h0, mut h1) = (blake3::hash(b""), blake3::hash(b""));
    for _ in 0..reps {
        let t0 = Instant::now();
        for &i in &order {
            let off = i * MIB;
            let end = (off + MIB).min(len);
            cv[i] = Hasher::new().set_input_offset(off as u64).update(&buf[off..end]).finalize_non_root();
        }
        h0 = root_of_pieces(&cv);
        let t1 = Instant::now();
        h1 = blake3::hash(&buf);
        let t2 = Instant::now();
        tt.push((t1 - t0).as_secs_f64());
        tr.push((t2 - t1).as_secs_f64());
    }
    assert_eq!(h0, h1, "roots disagree");
    println!("rust blake3 1.8.7 (runtime dispatch), {mib} MiB, {reps} reps, median");
    for (name, t) in [("tree", &mut tt), ("root", &mut tr)] {
        t.sort_by(f64::total_cmp);
        let m = t[reps / 2];
        println!("  {name:<4} {m:6.3} s  {:5.2} GiB/s", len as f64 / m / (1u64 << 30) as f64);
    }
}
