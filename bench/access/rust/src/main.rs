//! accbench — storage access-method benchmark, Rust implementation.
//!
//! A line-for-line peer of ../c/accbench.c: same methods, same syscalls, same
//! buffer size, same hash, same output. See ../README.md.
//!
//!   accbench vfs      MOUNTPOINT   method 1: lstat + open + read by path
//!   accbench handle   MOUNTPOINT   method 2: walk, name_to_handle_at, inode order, open_by_handle_at
//!   accbench bulkstat MOUNTPOINT   method 2: XFS_IOC_BULKSTAT + open_by_handle_at (xfsdump's way)
//!   accbench e2fs     DEVICE       method 3: libext2fs inode scan, own pread of data (dump's way)
//!   accbench raw      DEVICE       method 3: hand-written ext4 parser, own pread of data
//!   accbench rawsort  DEVICE       method 3: as raw, but all extents of all files in disk order

mod bulkstat;
mod common;
mod e2fs;
mod ext4;
mod handle;
mod sys;
mod vfs;

use std::io::Write;
use std::time::Instant;

use common::{Ctx, die};

const CSV_HEADER: &str = "impl,method,files,names,bytes,find_ns,read_ns,total_ns,utime_us,stime_us,\
                          minflt,majflt,inblock,nvcsw,nivcsw,maxrss_kb,combined";

fn main() {
    let (mut method, mut target, mut digest, mut hash) = (None, None, None, false);
    let mut args = std::env::args().skip(1);
    let mut bad = false;
    while let Some(a) = args.next() {
        match a.as_str() {
            "--header" => {
                println!("{CSV_HEADER}");
                return;
            }
            "--hash" => hash = true,
            "--digest" => match args.next() {
                Some(f) => {
                    digest = Some(f);
                    hash = true;
                }
                None => bad = true,
            },
            _ if method.is_none() => method = Some(a),
            _ if target.is_none() => target = Some(a),
            _ => bad = true,
        }
    }
    let (Some(method), Some(target), false) = (method, target, bad) else {
        eprintln!(
            "usage: accbench {{vfs|handle|bulkstat|e2fs|raw|rawsort}} TARGET [--hash] [--digest FILE]\n       accbench --header"
        );
        std::process::exit(2);
    };

    let mut ctx = Ctx::new(hash);
    let t0 = Instant::now();
    match method.as_str() {
        "vfs" => vfs::run(&mut ctx, &target),
        "handle" => handle::run(&mut ctx, &target),
        "bulkstat" => bulkstat::run(&mut ctx, &target),
        "e2fs" => e2fs::run(&mut ctx, &target),
        "raw" => ext4::run_raw(&mut ctx, &target),
        "rawsort" => ext4::run_rawsort(&mut ctx, &target),
        m => die(format_args!("unknown method {m}")),
    }
    let total = t0.elapsed().as_nanos() as u64;

    let mut ru: libc::rusage = unsafe { std::mem::zeroed() };
    unsafe { libc::getrusage(libc::RUSAGE_SELF, &mut ru) };
    let us = |t: libc::timeval| t.tv_sec * 1_000_000 + t.tv_usec;
    println!(
        "rust,{method},{},{},{},{},{},{total},{},{},{},{},{},{},{},{},{:016x}",
        ctx.files,
        ctx.names,
        ctx.bytes,
        total - ctx.read_ns,
        ctx.read_ns,
        us(ru.ru_utime),
        us(ru.ru_stime),
        ru.ru_minflt,
        ru.ru_majflt,
        ru.ru_inblock,
        ru.ru_nvcsw,
        ru.ru_nivcsw,
        ru.ru_maxrss,
        ctx.combined
    );

    if let Some(path) = digest {
        ctx.recs.sort_unstable_by_key(|r| r.ino);
        let f = std::fs::File::create(&path)
            .unwrap_or_else(|e| die(format_args!("create {path}: {e}")));
        let mut o = std::io::BufWriter::new(f);
        let w = (|| {
            writeln!(
                o,
                "# files={} names={} bytes={}",
                ctx.files, ctx.names, ctx.bytes
            )?;
            for r in &ctx.recs {
                writeln!(o, "{} {} {:016x}", r.ino, r.size, r.digest)?;
            }
            o.flush()
        })();
        if let Err(e) = w {
            die(format_args!("write {path}: {e}"));
        }
    }
}
