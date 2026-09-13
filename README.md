# Diavolo

A backup tool for Linux: a reimagining of UNIX `dump`, not a port, written in Rust.

> **UNSTABLE.** The archive format is not frozen until v0.1.0. Archives written
> before then may become unreadable by later builds. Any reader built from this
> repository **refuses** a format version it does not explicitly recognise. That
> loud refusal is the whole compatibility policy until v0.1.0. There is no promise.

## Why

`dump` is still the fastest thing on Linux that does genuinely correct incremental
backup of ext4. It is also the only one whose restore replays deletions. But its
format comes from the 1980s:

- no index;
- no content digests;
- no encryption;
- no documented guarantee for extended attributes;
- a restore path with three ways to silently do the wrong thing.

Diavolo keeps what dump gets right and rebuilds the rest.

## The name

*Diavolo* was a backup program for the Amiga, long dead and long forgotten. Reusing
the name is deliberate: this project revives technology that was abandoned too early.

That describes `dump` exactly. Most distributions dropped it, Red Hat and Fedora among
them, and for years it had no upstream release. Yet it remains the fastest correct
incremental backup on Linux, and the only one whose restore replays deletions.

## v0.1 scope

These five, and nothing else until they ship:

1. An immutable, append-only archive format with a real index.
2. BLAKE3 verification, over contents **and** over the index.
3. Restore that handles deletions correctly by construction, not by procedure.
4. A read-only FUSE mount over an archive chain.
5. ext4 support via `libext2fs`, reading the block device directly as `dump` does.

## Method

The format is specified first, and the specification is mechanical:

- `docs/spec/format.yaml` is the single authoritative input.
- `cargo xtask codegen` generates:
  - the byte-layout constants,
  - the error taxonomy,
  - the rendered documentation in `docs/generated/`,
  - the conformance corpus in `corpus/`.
- The reader and writer are written by hand, and each is tested against the
  corpus, **never against each other**.
- Every numbered rule has a broken archive in `corpus/broken/` that violates exactly
  that rule.
- CI regenerates everything and fails on any diff.

Design notes, including the options that were rejected, are in `docs/design/`.

## Licence

Dual-licensed under MIT or Apache-2.0, at your option.
