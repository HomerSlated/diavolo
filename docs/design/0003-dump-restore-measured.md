# 0003 — dump/restore deletion semantics, measured

Status: **evidence**, 2026-09-13. Resolves the "established by construction, not
measured" item in brief PART 12. It also corrects one claim in PART 4 gap 3.

## Setup

- `tools/dump-deletion-test.sh` was run as root by kgr.
- `/usr/local/sbin/{dump,restore}` **0.4b56**, with libext2fs 1.47.2.
  - **Not 0.4b52.** The brief and an earlier version of this note both say 0.4b52.
    That was true at 15:25, but the binaries were replaced at 16:27–16:29, before the
    17:31 run.
  - The new binaries report the same version as kgr's clone in `private/code/dump-code`
    (HEAD `9e6f839`, "Make 0.4b56 release", built 16:22). That is consistent with them
    being built from it. It is not proven: the installed binaries are smaller than
    the ones in the build tree, which fits stripping but was not verified.
- Two 64 MiB ext4 loop filesystems, each dump taken with the source remounted
  read-only.
- A level 0 dump, then these mutations, then a level 1 dump:
  - delete a file;
  - `rm -r` a directory;
  - delete and recreate a file;
  - remove one link of a hardlinked pair.

The failure-mode runs (B–D) were repeated without root on the same `L0.dump` and
`L1.dump` files, in plain directories. `restore` needs root only to `chown`.

## Results

| Run | Procedure | Outcome |
|---|---|---|
| A | `-r` L0, `-r` L1, same cwd | After L0, all removed paths exist and `replaced` = `original`. After L1, all are gone, `replaced` = `NEW`, and `link-a` has nlink 1. The user xattr and POSIX ACL round-trip. **Deletions replay.** |
| B | `-r` L0, then `-x` L1 | rc 0. Every deleted path is still present, and `replaced` = `NEW`. **Silent.** |
| C | `-r` L0, `rm restoresymtable`, `-r` L1 | rc 1: `cannot open symbol table file ./restoresymtable`. Nothing changed. **Loud.** |
| D | a stray file in the target, then `-r` L0 and `-r` L1 | rc 0, and the stray file survives. **Silent.** |

The fixed script checks the state after L0 before it trusts any "absent" verdict. The
first version of the script did not, so its deletion checks could have passed
vacuously. Run A closes that hole.

## Corrections to the brief

- **PART 12:** `restore -r` replaying deletions is now **measured**, together with the
  xattr and ACL round-trip (PART 4 gap 11).
- **PART 4 gap 3, failure mode 2 is loud, not silent.** Under `-r`, a missing
  `restoresymtable` is a hard error. It becomes silent only if the operator reacts by
  switching to `-x`, and that is failure mode 1. The 0.4b56 source agrees:
  `initsymtable()` calls `errx(1, …)` when the file cannot be opened.
  - There is a silent variant this test does **not** cover: a pass run from a cwd
    that contains a *different* `restoresymtable`. It is unmeasured.
- **Modes 1 and 3 are confirmed silent**, exactly as described.
- **All of the above is measured on 0.4b56 only.** Whether 0.4b52 behaves the same
  way is unmeasured, and its binary is gone. Two things still depend on 0.4b52:
  - the `strings` evidence in SOLUTIONS.md was taken from it;
  - the real 233 GiB chain was *written* by it.

  Restoring that chain with 0.4b56 is a cross-version restore that nobody has tested.

These corrections belong in `~/tmp/system/SOLUTIONS.md` §2026-09-13 and in the brief.
Those are owned elsewhere, so they are recorded here and reported to kgr, not edited.

## Consequences for Diavolo

- **Mode 1:** answered by 0001 D3. Every index is a complete namespace, so no restore
  mode or partial extraction can depend on a deletion replay.
- **Mode 2:** has no analogue, because there is no restore-side state carried between
  passes.
- **Mode 3:** *not* answered by the format. What a restore does with a non-empty
  target is tool behaviour. The spec was silent on it, and brief PART 7 rule 4 says
  silence is the failure mode. It is now rule `policy.restore_target`.

## Addendum — what says removal is intended, and the actual mechanism

Checked against kgr's 0.4b56 clone (`private/code/dump-code`, HEAD `9e6f839`).

**Intent, from strongest to weakest source:**

1. **`restore/restore.c` above `removeoldleaves()`.** This is inherited from 4.4BSD
   and is © 1983, 1993 The Regents of the University of California:

   > The following four routines implement the incremental restore algorithm. The
   > first removes old entries, the second does renames and calculates the
   > extraction list, the third cleans up link names missed by the first two, and
   > the final one deletes old directories.

2. **`restore/tape.c:374-375`.** The record dump writes first is called by restore's
   own error message the *file removal list*:
   `if (spcl.c_type != TS_CLRI) errx(1, "Cannot find file removal list");`
3. **Upstream bug #157** (<https://sourceforge.net/p/dump/bugs/157/>), fixed by
   `fe2d1f1`, Ben Harris, 2014, "restore: fix hang when dir is removed". A regression
   in removing deleted directories during incremental restore was treated as a bug
   and fixed. Removal is intended behaviour.
4. **Dave Martindale, comp.unix.wizards, 2 May 1986**
   (<https://www.tuhs.org/Usenet/comp.unix.wizards/1986-May/004579.html>):

   > When you restore an incremental dump over a lower-level dump, it has to delete
   > files that have been removed and relink things that have changed names.

   This is contemporary practitioner testimony, not documentation.

**Not a source:** `restore(8)`. The FreeBSD, NetBSD, OpenBSD and Linux pages say only
that incrementals are "layered on top" and that `restoresymtable` passes information
between passes. None of them states that deleted files are removed.

**The mechanism.** It corrects SOLUTIONS.md and brief PART 8.1, which attribute
removal to diffing directory entry lists.

- **Every dump carries the complete used-inode map, regardless of level.**
  - `dump/main.c:909` writes `dumpmap(usedinomap, TS_CLRI, …)`.
  - `removeoldleaves()` (`restore.c:208-226`) marks REMOVE every inode that is in the
    symbol table but absent from that map.
  - Dumped directory listings then handle names: renames and new links in
    `nodeupdates()`, and dropped links to still-live inodes in `findunreflinks()`,
    such as the hardlink case in run A.
- **Completeness is therefore per dump for inode existence, and chain-carried for
  names.** The design lesson in 0001 D3 stands, and is if anything reinforced: dump's
  only complete positive record is an inode *bitmap* in every file.
- **The `nodump` trap, located exactly.** `dump/traverse.c:299` and `:616-619` clear
  excluded inodes from `usedinomap`. An excluded file therefore reaches restore
  looking exactly like a deleted one. This is brief PART 4 gap 13, and the reason
  0001 D4 makes exclusion a positive record.
