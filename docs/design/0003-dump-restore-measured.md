# 0003 — dump/restore deletion semantics, measured

Status: **evidence**, 2026-09-13. Resolves the "established by construction, not
measured" item in brief PART 12. It also corrects one claim in PART 4 gap 3.

## Setup

- `tools/dump-deletion-test.sh` was run as root by kgr.
- `/usr/local/sbin/{dump,restore}` 0.4b52, with libext2fs 1.47.2.
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
