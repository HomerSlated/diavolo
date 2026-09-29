//! Rust peer of ../b3min.c: minimal b3sum (one file, one thread) via
//! blake3::hazmat, the binary-size probe.
use blake3::hazmat::{ChainingValue, HasherExt, Mode, merge_subtrees_non_root, merge_subtrees_root};
use blake3::Hasher;
use std::io::Read;

const MIB: usize = 1 << 20;

fn merge(cv: &[ChainingValue]) -> ChainingValue {
    if cv.len() == 1 {
        return cv[0];
    }
    let k = 1 << (usize::BITS - 1 - (cv.len() - 1).leading_zeros());
    merge_subtrees_non_root(&merge(&cv[..k]), &merge(&cv[k..]), Mode::Hash)
}

fn main() {
    let path = std::env::args().nth(1).expect("file");
    let mut f = std::fs::File::open(&path).expect("open");
    let mut buf = vec![0u8; MIB];
    let mut cv: Vec<ChainingValue> = Vec::new();
    let root = loop {
        let mut n = 0;
        while n < MIB {
            match f.read(&mut buf[n..]).expect("read") {
                0 => break,
                r => n += r,
            }
        }
        if cv.is_empty() && n < MIB {
            break blake3::hash(&buf[..n]); // the whole file is one piece: it is the root
        }
        if n > 0 {
            let off = (cv.len() * MIB) as u64;
            cv.push(Hasher::new().set_input_offset(off).update(&buf[..n]).finalize_non_root());
        }
        if n < MIB {
            let k = 1 << (usize::BITS - 1 - (cv.len() - 1).leading_zeros());
            break merge_subtrees_root(&merge(&cv[..k]), &merge(&cv[k..]), Mode::Hash);
        }
    };
    println!("{}  {}", root.to_hex(), path);
}
