//! Rust peer of ../sortbench.c: the same dumped plans, the same checks.
//!   unstable  slice::sort_unstable_by_key (ipnsort), what accbench uses
//!   stable    slice::sort_by_key (driftsort: merges natural runs)
//!   radix     a port of the C radix sort, so Rust gets the same idea
//! Usage: sortbench PLAN... [-r REPS]
use std::time::Instant;

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(C)]
struct Piece {
    pblk: u64,
    nblk: u32,
    file: u32,
    ext: u32,
    off: u32,
}

fn is_sorted(a: &[Piece]) -> bool {
    a.windows(2).all(|w| w[0].pblk <= w[1].pblk)
}

fn radix(a: &mut [Piece], tmp: &mut [Piece]) {
    if is_sorted(a) {
        return;
    }
    let n = a.len();
    let all = a.iter().fold(0, |m, x| m | x.pblk);
    let bits = 64 - (all | 1).leading_zeros() as usize;
    let passes = bits.div_ceil(11);
    let w = bits.div_ceil(passes);
    let mask = (1u64 << w) - 1;
    let mut h = [[0u32; 2048]; 6];
    for x in a.iter() {
        for (p, hp) in h.iter_mut().enumerate().take(passes) {
            hp[((x.pblk >> (p * w)) & mask) as usize] += 1;
        }
    }
    let (mut src, mut dst): (&mut [Piece], &mut [Piece]) = (a, tmp);
    let mut swapped = false;
    for (p, c) in h.iter_mut().enumerate().take(passes) {
        if c[((src[0].pblk >> (p * w)) & mask) as usize] as usize == n {
            continue; // every key has the same digit here
        }
        let mut sum = 0;
        for d in c.iter_mut().take(mask as usize + 1) {
            let k = *d;
            *d = sum;
            sum += k;
        }
        for x in src.iter() {
            // SAFETY: the digit is masked to < 2^w <= 2048 = c.len(), and the
            // prefix sums over n keys keep every *d < n = dst.len().
            unsafe {
                let d = c.get_unchecked_mut(((x.pblk >> (p * w)) & mask) as usize);
                *dst.get_unchecked_mut(*d as usize) = *x;
                *d += 1;
            }
        }
        std::mem::swap(&mut src, &mut dst);
        swapped = !swapped;
    }
    if swapped {
        dst.copy_from_slice(src); // dst is the caller's slice again
    }
}

fn main() {
    let mut reps = 51;
    let mut files = Vec::new();
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        if a == "-r" {
            reps = args.next().unwrap().parse().unwrap();
        } else {
            files.push(a);
        }
    }
    println!("{:<30} {:>7} {:>9} {:>9} {:>9}   (median us)", "plan", "n", "unstable", "stable", "radix");
    for path in files {
        let bytes = std::fs::read(&path).unwrap();
        let n = bytes.len() / size_of::<Piece>();
        let orig: Vec<Piece> = (0..n)
            .map(|i| {
                let b = &bytes[i * 24..];
                let u32at = |o: usize| u32::from_ne_bytes(b[o..o + 4].try_into().unwrap());
                Piece {
                    pblk: u64::from_ne_bytes(b[..8].try_into().unwrap()),
                    nblk: u32at(8),
                    file: u32at(12),
                    ext: u32at(16),
                    off: u32at(20),
                }
            })
            .collect();
        let mut want = orig.clone();
        want.sort_unstable_by_key(|x| x.pblk);
        let name = path.rsplit('/').next().unwrap();
        print!("{:<30.30} {:>7}", name, n);
        let mut a = orig.clone();
        let mut tmp = orig.clone(); // scratch, sized once per plan as in C
        for alg in 0..3 {
            let mut t: Vec<u128> = (0..reps)
                .map(|_| {
                    a.copy_from_slice(&orig);
                    let t0 = Instant::now();
                    match alg {
                        0 => a.sort_unstable_by_key(|x| x.pblk),
                        1 => a.sort_by_key(|x| x.pblk),
                        _ => radix(&mut a, &mut tmp),
                    }
                    let dt = t0.elapsed().as_nanos();
                    assert!(a == want, "wrong result");
                    dt
                })
                .collect();
            t.sort();
            print!(" {:>9.1}", t[reps / 2] as f64 / 1e3);
        }
        println!();
    }
}
