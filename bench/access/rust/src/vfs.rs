//! Method 1: VFS. lstat + open + read by name, directory by directory.

use std::ffi::{CStr, CString, c_int};

use crate::common::{Ctx, clear_errno, die, die_os, dot_or_dotdot, errno};

fn dir(ctx: &mut Ctx, dfd: c_int) {
    // SAFETY: dfd is an open directory fd we own; fdopendir takes it over.
    let d = unsafe { libc::fdopendir(dfd) };
    if d.is_null() {
        die_os("fdopendir");
    }
    loop {
        clear_errno();
        // SAFETY: d is a live DIR*; the entry stays valid until the next readdir on d.
        let e = unsafe { libc::readdir(d) };
        if e.is_null() {
            if errno() != 0 {
                die_os("readdir");
            }
            break;
        }
        let (name, dtype) = unsafe { (CStr::from_ptr((*e).d_name.as_ptr()), (*e).d_type) };
        if dot_or_dotdot(name.to_bytes()) {
            continue;
        }
        ctx.names += 1;
        if dtype != libc::DT_DIR && dtype != libc::DT_REG && dtype != libc::DT_UNKNOWN {
            continue;
        }
        let pfd = unsafe { libc::dirfd(d) };
        // lstat first, as tar and rsync do: metadata, and hardlink detection before opening
        let mut st: libc::stat = unsafe { std::mem::zeroed() };
        if unsafe { libc::fstatat(pfd, name.as_ptr(), &mut st, libc::AT_SYMLINK_NOFOLLOW) } != 0 {
            die_os(format_args!("fstatat {}", name.to_string_lossy()));
        }
        match st.st_mode & libc::S_IFMT {
            libc::S_IFDIR => {
                let flags = libc::O_RDONLY | libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC;
                let c = unsafe { libc::openat(pfd, name.as_ptr(), flags) };
                if c < 0 {
                    die_os(format_args!("openat dir {}", name.to_string_lossy()));
                }
                dir(ctx, c);
            }
            libc::S_IFREG if ctx.seen.add(st.st_ino) => {
                let flags = libc::O_RDONLY | libc::O_NOFOLLOW | libc::O_CLOEXEC;
                let f = unsafe { libc::openat(pfd, name.as_ptr(), flags) };
                if f < 0 {
                    die_os(format_args!("openat {}", name.to_string_lossy()));
                }
                let n = ctx.read_stream(f, st.st_ino, st.st_size as u64);
                if n != st.st_size as u64 {
                    die(format_args!(
                        "{}: read {n} of {} bytes",
                        name.to_string_lossy(),
                        st.st_size
                    ));
                }
                if !ctx.hash {
                    ctx.add_record(st.st_ino, n, [0; 32]);
                }
                unsafe { libc::close(f) };
            }
            _ => {}
        }
    }
    unsafe { libc::closedir(d) };
}

pub fn run(ctx: &mut Ctx, mnt: &str) {
    let c = CString::new(mnt).unwrap();
    let fd = unsafe {
        libc::open(
            c.as_ptr(),
            libc::O_RDONLY | libc::O_DIRECTORY | libc::O_CLOEXEC,
        )
    };
    if fd < 0 {
        die_os(format_args!("open {mnt}"));
    }
    dir(ctx, fd);
}
