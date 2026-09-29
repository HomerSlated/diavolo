//! pool: a Rust port of ../../levelup/pool/pool.c. Persistent std threads and
//! one operation, a parallel for-loop over [0, n). Workers claim indices with
//! a relaxed fetch-add, one at a time; the caller works too. A job is
//! published and collected under a mutex, so the only atomic is the index.
//! Idle workers sleep on a condvar at once.
//!
//! Why not rayon: its idle workers spin and sched_yield before sleeping, and
//! on an SMT CPU that slowed accbench's reading thread by up to 6 % at 12
//! threads; this pool matched C exactly, and is 57 KB smaller stripped. See
//! ../../levelup/README.md, "In accbench".

use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;

type Task = *const (dyn Fn(usize) + Sync);

struct State {
    task: Option<(Task, usize)>,
    generation: u64,
    busy: usize,
    quit: bool,
}

// SAFETY: the task pointer is only dereferenced while run(), which borrows the
// closure, waits for every worker to be done with it.
unsafe impl Send for State {}

struct Shared {
    mu: Mutex<State>,
    go: Condvar,
    done: Condvar,
    next: AtomicUsize,
}

pub struct Pool {
    sh: Arc<Shared>,
    workers: Vec<JoinHandle<()>>,
}

fn claim(sh: &Shared, task: Task, n: usize) {
    // SAFETY: see State; the closure outlives this call.
    let f = unsafe { &*task };
    loop {
        let i = sh.next.fetch_add(1, Ordering::Relaxed);
        if i >= n {
            break;
        }
        f(i);
    }
}

fn worker(sh: Arc<Shared>) {
    let mut seen = 0;
    let mut st = sh.mu.lock().unwrap();
    loop {
        while st.generation == seen && !st.quit {
            st = sh.go.wait(st).unwrap();
        }
        if st.quit {
            return;
        }
        seen = st.generation;
        let (task, n) = st.task.unwrap();
        drop(st);
        claim(&sh, task, n);
        st = sh.mu.lock().unwrap();
        st.busy -= 1;
        if st.busy == 0 {
            sh.done.notify_one();
        }
    }
}

/// A slice's base pointer, shareable because every index is written by
/// exactly one claimant.
struct Slots<T>(*mut T);
// SAFETY: see above; T crosses threads, so it must be Send.
unsafe impl<T: Send> Sync for Slots<T> {}

impl<T> Slots<T> {
    /// A method, not a field access in the closure: closures capture fields
    /// precisely, and a bare *mut T is not Sync.
    ///
    /// SAFETY: i is in bounds and no other thread writes slot i.
    unsafe fn put(&self, i: usize, v: T) {
        unsafe { self.0.add(i).write(v) }
    }
}

impl Pool {
    /// threads counts the caller: Pool::new(1) runs everything inline.
    pub fn new(threads: usize) -> Pool {
        let sh = Arc::new(Shared {
            mu: Mutex::new(State {
                task: None,
                generation: 0,
                busy: 0,
                quit: false,
            }),
            go: Condvar::new(),
            done: Condvar::new(),
            next: AtomicUsize::new(0),
        });
        let workers = (1..threads.max(1))
            .map(|_| {
                let sh = Arc::clone(&sh);
                std::thread::spawn(move || worker(sh))
            })
            .collect();
        Pool { sh, workers }
    }

    /// f(i) for every i in [0, n), on all the pool's threads.
    pub fn run(&self, n: usize, f: &(dyn Fn(usize) + Sync)) {
        if self.workers.is_empty() {
            (0..n).for_each(f);
            return;
        }
        // SAFETY: erases f's lifetime; we don't return until no worker holds it.
        let task: Task = unsafe { std::mem::transmute::<&(dyn Fn(usize) + Sync), Task>(f) };
        {
            let mut st = self.sh.mu.lock().unwrap();
            st.task = Some((task, n));
            self.sh.next.store(0, Ordering::Relaxed);
            st.busy = self.workers.len();
            st.generation += 1;
            self.sh.go.notify_all();
        }
        claim(&self.sh, task, n);
        let mut st = self.sh.mu.lock().unwrap();
        while st.busy != 0 {
            st = self.sh.done.wait(st).unwrap();
        }
        st.task = None;
    }

    /// out[i] = f(i) for every i, in parallel.
    pub fn map_into<T: Copy + Send>(&self, out: &mut [T], f: &(dyn Fn(usize) -> T + Sync)) {
        let slots = Slots(out.as_mut_ptr());
        // SAFETY: i < out.len(), and each i is claimed exactly once.
        self.run(out.len(), &|i| unsafe { slots.put(i, f(i)) });
    }
}

impl Drop for Pool {
    fn drop(&mut self) {
        self.sh.mu.lock().unwrap().quit = true;
        self.sh.go.notify_all();
        for w in self.workers.drain(..) {
            let _ = w.join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::Pool;

    #[test]
    fn every_index_once() {
        for threads in [1, 2, 5, 12] {
            let pool = Pool::new(threads);
            for n in [0, 1, 7, 1000] {
                let mut out = vec![0usize; n];
                pool.map_into(&mut out, &|i| i * 3 + 1);
                assert!(out.iter().enumerate().all(|(i, &v)| v == i * 3 + 1));
            }
        }
    }
}
