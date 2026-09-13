# 0001 — Archive layout

Status: **draft**, format 0.1 (UNSTABLE). Supersedes nothing.

This note records the decisions that `docs/spec/format.yaml` encodes. It also records
the options that were rejected, and why. The YAML is authoritative for bytes; this note
is authoritative for *reasons*. Where they disagree, the YAML is right and this note
is stale.

Brief references are to `~/Documents/diavolo-project-brief.md`.

## D1. One archive file is one capture of one filesystem

A capture is one frozen snapshot, read once. Its archive is written front to back and
never modified afterwards.

An incremental capture is also a complete archive. It holds some file contents itself
and references the rest in named ancestor archives.

**Rejected: multiple captures appended into one file.** Every capture would share the
fate of one file on one borrowed drive (PART 4 gap 9). It also turns deleting an old
chain into rewriting a file.

## D2. Physical layout: header, blocks, index, trailer

```
offset 0        HEADER    fixed 32 bytes, written first, never rewritten
                BLOCK*    self-framed: 56-byte block header + payload
index_offset    INDX      one block whose payload is the index
EOF - 160       TRAILER   fixed 160 bytes, the root of trust
```

- **Endianness.** Every integer is little-endian.
- **Offsets.** Every offset is an unsigned count of bytes from the first byte of the
  archive file.

The trailer is found at a fixed distance from EOF, so it never has to be searched
for. That is the `b3trailer.h` design: a fixed length means a fixed offset, so no
payload byte can be mistaken for the trailer. Nothing is ever patched back into the
header (PART 8.3).

The design is shared with `b3trailer.h`; the **bytes are not**. `b3trailer.h`'s trailer
is 72 ASCII bytes, and this one is 160 binary bytes. `b3read` cannot verify a Diavolo
archive, and must not be expected to.

**Fields that appear in more than one place.** `format_major`, `format_minor` and
`archive_uuid` are in both the header and the trailer. That lets either one be parsed,
and salvaged, alone. Each duplicated pair has an equality rule and a broken sibling in
which the two differ.

The trailer seals `index_len` along with `index_offset`. Its equations are:

- `index_offset + BLOCK_HEADER_LEN + index_len + TRAILER_LEN == archive_len`
  (the INDX block immediately precedes the trailer);
- `INDX block payload_len == trailer.index_len`;
- `INDX block payload_digest == trailer.index_digest`.

**The magic numbers follow PNG's pattern.** They are `89 'D' 'V' 'L' 0D 0A 1A 0A` for
the header and `89 'D' 'V' 'T' 0D 0A 1A 0A` for the trailer. The pattern catches the
three classic transfer corruptions:

- a 7-bit channel strips the high bit of `0x89`;
- CRLF translation mangles the `0D 0A`;
- a DOS text-mode reader stops at `1A`.

A backup medium passes through more of those tools than a PNG does.

**`prev_trailer_offset` is reserved.** It must be zero in format 0.x. It exists so that
a later minor version can append checkpoint trailers without a layout change.
Checkpointing is not in v0.1: a checkpoint of a half-read filesystem cannot claim a
complete namespace (D3).

## D3. Every index is a complete namespace snapshot

**This is the decision that answers PART 8.1.**

Every archive's index lists every inode alive in the snapshot. It also lists the
**complete entry list of every directory**, including directories untouched since the
ancestor archive.

An incremental saves on *content*: unchanged files are recorded as
`content_kind = inherited`, a reference to an ancestor archive by
`(chain index, ino, content digest)`. It never saves on *namespace*.

Consequences:

- **Deletion needs no replay.** The newest index *is* the manifest of what should
  exist. Restoring archive N means materialising index N and fetching inherited
  contents from ancestors.
  - There is no `restoresymtable`.
  - Nothing depends on the order of passes.
  - None of the three silent failures in PART 4 gap 3 can occur.

  This is SOLUTIONS.md's own fallback ("declarative beats replay") turned into the
  format.
- **Completeness belongs to a single file, not to the chain.** A missing ancestor is
  detected, and fails loudly. It cannot make deleted files reappear.
- **Cost.** The index is O(all inodes) per incremental, not O(changed inodes).
  - Estimate for the current system (904,049 paths): roughly 150 MiB uncompressed
    per archive, from the record sizes in D5 plus names. ***Inference***: not yet
    measured.
  - dump's `.daf` for the L0 is on the same order.
  - This is noise against a full, but not against a quiet incremental; see D5 and
    open question 2.

**The trap stays shut in writing.** Every directory record carries `completeness`:

| Value | Meaning |
|---|---|
| `0` | invalid |
| `1` | complete |
| `2` | partial — registered, undefined |

Every directory record also carries a tombstone range. In 0.x:

- a writer MUST write `complete`;
- a reader MUST reject `partial`;
- tombstone counts MUST be zero.

If a later version optimises large unchanged directories, the field that makes that
safe is already in every file. It would be impossible to add afterwards.

**Rejected: "dir unchanged, see ancestor" records in 0.1.** That form is positive
and verifiable, provided it carries the ancestor record's digest. But it
re-introduces chain-dependent namespace resolution. Reserve it; don't build it.

**Rejected: storing only changed directories, dump-style.** This is the design
PART 8.1 identifies as correct only by procedure.

## D4. Exclusion is a positive record

A file whose content was deliberately not captured still gets:

- its directory entry;
- its inode record, with `content_kind = excluded`.

The file might be skipped because of a `nodump` flag or a policy. PART 4 gap 13 is
exactly the case where an exclusion would otherwise look like a deletion.

A v0.1 writer implements no exclusions, but the value is defined now. That keeps
"excluded" from ever meaning absent.

## D5. The index is fixed-size tables plus a byte heap, uncompressed

The index payload has a fixed-size index header, then these tables in this fixed
order, each a run of fixed-size records:

1. chain
2. inode
3. dir
4. dirent
5. extent
6. xattr
7. tombstone (always empty in 0.x)
8. byte heap

**Table positions are implicit.** They are computed from the counts in the index
header and the record sizes, so there are no offset fields to disagree with the
counts. The payload length satisfies one equation:

`payload_len = INDEX_HEADER_LEN + Σ(count × record_len) + heap_len`

Sort orders are normative:

| Table | Sorted by | Uniqueness |
|---|---|---|
| inodes | `ino` | strictly ascending |
| dirents | `(parent_ino, name bytes)` | — |
| extents | `(ino, file_offset)` | — |
| xattrs | `(ino, name bytes)` | — |

Point lookups are therefore binary searches over an `mmap`: FUSE needs no in-memory
tree and no decompression.

**Uncompressed**, so that FUSE and listing can binary-search it zero-copy through
`mmap`, with no decompression anywhere on the metadata path.

That trade is **not** cheap on every archive. The index is about 0.06% of a 233 GiB
full, about 0.5% of a 30 GiB incremental, and could be most of a quiet daily
incremental. The size risk is tracked in open question 2. `index_codec` exists with
only `1 = stored` defined, so a compressed index can be added later without a layout
change.

### The heap

- **Addressing.** A heap reference is an `(offset, len)` pair. `offset` is a count of
  bytes from the first byte of the heap, not from the index or the file.
- **Bounds.** `offset + len <= heap_len` is a rule, and the check MUST NOT overflow.
- **Empty strings.** A zero-length reference MUST have `offset = 0`, so there is one
  encoding of the empty string.
- **Sharing.** Several references MAY address the same bytes. Readers must not assume
  that references are unique or do not overlap.
- **Unreferenced bytes** are permitted, and carry no meaning. A writer SHOULD NOT emit
  them. This is stated so that neither "the heap is minimal" nor "padding is an error"
  is ever inferred from silence.

**Rejected: serde formats (CBOR, bincode, postcard).** Their byte layout is defined by
a library version, not by this spec. "The layout lives in `format.yaml` and nowhere
else" would then be false on day one.

### Values that are derived, never stored

These have been removed from the format, not merely constrained (PART 8.4). A field
that can be derived cannot disagree with its source.

- `nlink`: count the dirents naming the inode.
- The dirent's file type: read it from the child inode's `mode`.
- A directory's `size`: meaningless for restore, and MUST be zero.
- Table offsets: see above.

Every remaining pair of fields that describe one quantity has an equation, and each
equation is a numbered rule in `rules.yaml`.

## D6. Content: DATA blocks of at most 1 MiB of raw bytes, one file per block

A DATA block's payload contains:

- a fixed data header: `ino`, `file_offset`, `raw_len`, `codec`, and `raw_digest`
  (BLAKE3 of the uncompressed bytes);
- the stored bytes.

The index's extent record repeats `raw_len` and `raw_digest`, and adds `block_offset`.

- **Why the index repeats them.** The index is sealed by the trailer; block headers
  are not. Trust flows from the trailer to the index to the chunk, never from the
  block header. An equation requires the two copies to agree.
- **Why the block carries its own `ino` and `file_offset`.** If the index or trailer
  is destroyed, file contents can still be salvaged by inode from a linear scan.
  Names are lost, as with `lost+found`, but contents are not. Salvage is not in
  v0.1. The format permits it.
- **Holes.** A byte range with no extent reads as zeros. The extent list of a file
  with `content_kind = extents` is complete by rule, so absence here is evidence
  (PART 8.1).
- **Codecs.**

  | Value | Codec |
  |---|---|
  | `0` | invalid |
  | `1` | stored |
  | `2` | zstd (RFC 8878 frame) |

  libzstd's output is not byte-stable across library versions, so:

  - **The writer's zstd conformance** is "decodes to the stated raw bytes and
    digest", never "byte-identical".
  - **The corpus generator never calls libzstd.** Its zstd fixtures are frames built
    only from `Raw_Block` and `RLE_Block` blocks, assembled by hand from RFC 8878.
    They are deterministic by construction and still genuine zstd frames that any
    decoder must accept. That keeps the CI diff gate over `corpus/` total.
  - A fixture with a real `Compressed_Block` may be added later. Its frame bytes are
    then a **declared spec input** under `docs/spec/`, which the generator copies
    into the corpus. They must not live under `corpus/`: the generator owns that
    directory outright and deletes anything it did not produce.
- **Packing many small files into one block** is a later block type. The extent
  record's `offset_in_block` field (MUST be 0 for DATA) exists so that it needs no
  layout change.

## D7. Integrity: what is sealed, and what the policy is (PART 8.2, 8.8)

The chain of trust:

| Stored in | Covers |
|---|---|
| trailer | its own digest over its first 128 bytes |
| trailer | `header_digest` = BLAKE3(header) |
| trailer | `index_digest` = BLAKE3(INDX payload) |
| INDX payload | every extent's `raw_digest` |
| INDX payload | every inode's `content_digest` = BLAKE3 of the file's full logical bytes, holes as zeros |

**The Merkle root over the index is BLAKE3's own tree.** BLAKE3 is already a Merkle
tree over 1 KiB chunks, and `index_digest` is its root. Verified streaming of a byte
range (the Bao construction) is available later without a format change. A second,
record-level tree would add a layout for no v0.1 benefit.

**Block headers carry an 8-byte `header_check`.** It is the first 8 bytes of
BLAKE3 over the header's first 48 bytes. It is an error-detection code for linear
scans and salvage, not a trust anchor.

**Integrity, not authenticity.** No key is involved, so anyone who can write the
archive can re-seal it. Authenticity belongs with encryption (v2). This is stated
normatively so nobody reads `verify: OK` as "not tampered with".

**Checksum policy, decided once.** There is no warn-and-continue mode anywhere.

| Mismatch in | Consequence |
|---|---|
| trailer, header or index | The archive is rejected. |
| a DATA block | That file is unverified. FUSE returns `EIO` for any read touching the chunk. Restore does not create the file, continues with the other files, and exits non-zero naming every failure. |

## D8. Version clocks and acceptance (PART 7 rule 1)

There are two independent clocks. Each is refused loudly when unrecognised.

| Clock | Lives in | Meaning | Refused by |
|---|---|---|---|
| `format_version` (major.minor) | archive header and trailer | byte layout of archives | the reader |
| `spec_schema` (integer) | `format.yaml` | vocabulary of the YAML itself | the generator |

The acceptance policy is a **table** in `format.yaml`, and a test pins it against the
golden files.

- **Major 0 is the UNSTABLE series.** Every minor version is a breaking change, and a
  reader accepts only the exact `(0, minor)` pairs listed.
- **Format 1.0 is frozen when Diavolo v0.1.0 ships.** From then on:
  - a higher minor within a known major is warn-and-read;
  - an unknown major is rejected.

Forward-compatibility primitives, enforced by the first reader (PART 8.5):

- **Skip flag.** Block flag bit 0, `SKIP`, marks an optional block: a reader that does
  not know the block type ignores it. A reader MUST reject an unknown block type
  without `SKIP`.
- **Even/odd flag bits.** Every flags field declared `even_odd` in the YAML follows
  one rule: an unknown even bit is ignored, and an unknown odd bit rejects the file.
- **Registered-but-undefined type ids.** A writer MUST NOT write them, and a reader
  treats them as unknown:
  - `CRYP`: encrypted segment
  - `KDFP`: key-derivation parameters
  - `PAR2`: recovery data
  - `PACK`: multi-file data block
- **Explicit invalid values.** Every enum has `0 = invalid`. Where "not recorded" is
  meaningful it is a separate value or presence bit, never a shared zero (PART 8.6).
  Example: inode `present` bits for nanoseconds and `crtime`, because 128-byte ext4
  inodes have neither.

## D9. Media-absent index: the sidecar is a strict sub-sequence of the archive

The sidecar replaces dump's `.daf`. Its bytes are exactly the archive's header, then
its INDX block, then its trailer, copied verbatim. Nothing in it is re-encoded or
re-sealed.

A reader classifies the file only **after** the trailer's own digest verifies, and
only from sealed trailer fields:

- **Archive:** `file_size == trailer.archive_len`.
- **Sidecar:** otherwise, if
  `file_size == HEADER_LEN + BLOCK_HEADER_LEN + trailer.index_len + TRAILER_LEN`.
  Its INDX block begins at `HEADER_LEN`.
- **Anything else is rejected.** This includes a truncated archive: its size matches
  neither equation.

**The two equations can both hold, and that is not a collision.** An archive with no
DATA blocks satisfies both. Examples:

- an incremental in which every file is `content_kind = inherited`;
- a capture containing only directories and empty files.

Such an archive is byte-identical to its own sidecar, so the two are one file with
one meaning. It is classified as an **archive**. It has no stored extents, so a
content read can never reach "content not present". The corpus contains an
all-inherited incremental to pin this.

Every digest still verifies, because no sealed byte changed. Listing, and FUSE
metadata operations, work with the media absent. Content reads fail with a precise
"content not present in sidecar" error, not a digest error.

**Rejected: a `file_kind` field in the header.** The sidecar's header would then
differ from the archive's, and so would `header_digest`. Either the sidecar needs its
own sealing, or the header digest must exclude a field. Both are worse than an
equation.

## D10. Timestamps, identifiers, strings

- **Timestamps are signed `i64` seconds plus `u32` nanoseconds.** ext4's extended
  timestamps span 1901–2446; unsigned seconds would silently corrupt pre-1970 times.
  Nanoseconds MUST be < 10⁹ (a two-stage check: in type, then in range). `crtime` and
  the nanosecond fields are gated by presence bits.
- **Names, symlink targets and xattr names/values are byte strings in the heap.**
  None is required to be UTF-8.
- **Dirent names** MUST be non-empty, contain no `/` or NUL, and not be `.` or `..`.
- **Xattr values are the Linux VFS representation**, as `getxattr(2)` returns them.
  POSIX ACLs are therefore `system.posix_acl_access` in `posix_acl_xattr` form, not
  ext4's on-disk `ext4_acl` form.
- **`mode` uses Linux `S_IFMT` values.**
- **`root_ino` is stored, never assumed to be 2.** At least one golden fixture uses
  a root inode that is not 2 (PART 8.7).

## D11. Writer capability stamp (PART 8.1, "an old writer's silence")

The index header carries:

- a `writer` string, e.g. `diavolo 0.0.0`;
- a `capabilities` bitmask of what the writer asserts it captured **for every inode**:
  - xattrs;
  - `fs_flags`;
  - `crtime` where the source has it.

A reader distinguishes "no xattrs" from "this writer did not look" by that bit, never
by an empty range.

## Open questions for kgr

1. **Maximum chunk size, 1 MiB.** Bigger chunks compress better. Smaller ones give
   cheaper random reads through FUSE. 1 MiB is conventional; 4 MiB is defensible.
2. **The index is O(all inodes) per incremental (D3).** This is the price of
   deletion-correctness living in the format. If the measured index size for the real
   filesystem comes in much above the ~150 MiB estimate, reconsider the reserved
   "inherit directory record" form. Do not reconsider completeness.
3. **Static musl builds.** They are not possible with the current toolchain (xbps
   `rustc`, no `rustup`, only the `x86_64-unknown-linux-gnu` target). This affects
   rescue media, not the format, so it is deferred but must be decided before rescue
   media work starts.
