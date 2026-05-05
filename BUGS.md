# BUGS

Static review as of 2026-05-05.  The defects found in this pass have
been fixed in the working tree; this file records what was found and how
it was addressed.

Checks used during the review:

- `make objs`
- `make lib`
- `cc --analyze` on the FUSE-independent wrapper sources
- `git diff --check`

`make bin` was not run locally because this workstation does not expose
FUSE headers through pkg-config.

## Fixed In This Pass

### High: `direct_dirid()` over-copied the `.DIR;1` suffix

Location: `ods2lib/direct.c`

`direct_dirid()` used the total descriptor length (`len + 6`) as the
copy size for the literal `.DIR;1`, reading past the string literal and
potentially writing past `nambuf`.

Fix: copy only the suffix length and validate `len + suffix_len` before
writing into `nambuf`.

### High: long directory specs could abort the FUSE process

Location: `ods2lib/direct.c`, `src/lookup.c`

User-controlled FUSE paths could produce a VMS directory string longer
than `direct_dirid()` accepted, and that path called `abort()`.

Fix: return `SS$_BADFILENAME` instead of aborting, and make POSIX-to-VMS
directory conversion fail on truncation instead of silently shortening
the path.

### High: default multithreaded FUSE mode raced global caches

Location: `src/fuse_ods2.c`

The code recommended `-s`, but did not require it.  ods2lib caches and
the textmode decode cache are global mutable state without locking.

Fix: inject `-s` into the libfuse argument vector so the filesystem runs
single-threaded until the cache layers are made thread-safe.

### Medium: long legal names could be truncated in `readdir()`

Location: `src/ops.c`

The display buffer was 80 bytes, but a legal versioned ODS-2 name can be
`40 + "." + 40 + ";" + 5` characters.

Fix: use a buffer sized for the real exposed maximum and report the same
limit through `statfs()`.

### Medium: `readdir()` was not resumable

Location: `src/ops.c`

`readdir()` ignored the incoming offset and passed offset `0` for every
entry, which can break large directories when libfuse needs multiple
calls to drain the listing.

Fix: add stable monotonically increasing offsets for `.`, `..`, and
directory entries; honor the incoming offset; stop cleanly when
`filler()` reports a full buffer.

### Medium: corrupt directory records looked like successful short listings

Location: `src/lookup.c`

Directory parser validation failures broke out of the current block but
still returned `SS$_NORMAL`.

Fix: return `SS$_BADIRECTORY` when record size, record extent, or name
extent checks fail.

### Medium: malformed HEAD timestamps could read outside the header block

Location: `src/lookup.c`

`fh2$b_idoffset` was checked only against the lower fixed-header bound.
A malformed offset near the end of the 512-byte HEAD could make timestamp
reads go out of bounds.

Fix: validate `idoffset * 2 + sizeof(struct IDENT) <= sizeof(struct HEAD)`
before reading the IDENT area.

### Low: `/000000` did not map to the root directory

Location: `src/lookup.c`

The comment promised `/000000` behaved like `/`, but only `/` was special
cased.

Fix: treat both `/` and `/000000` as the MFD.

### Low: smoke test raced first unmount against the second mount

Location: `test/smoke.sh`

The first foreground FUSE process was unmounted but not waited for before
starting the second mount.

Fix: wait for the first background FUSE process after `fusermount3 -u`.
