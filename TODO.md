# TODO

Execution checklist for `fuse-ods2`.

## Locked-in design decisions

- **Read-only**: every write op returns `EROFS`.
- **File versions** (`NAME.EXT;n`): default shows only the latest version.
  `-o allversions` exposes every version.
- **Case**: default keeps the on-disk uppercase. `-o lower` for lowercase.
- **Record format**: default returns raw bytes. `-o textmode` strips
  VAR/VFC record headers and converts to LF.
- **Container**: raw images only in the MVP. SimH `.dsk` files are raw,
  so they are supported for free. VHD is out of scope.
- **Volume sets**: supported via `-o vol=disk1.img,disk2.img,...`.
- **Partition offset**: supported via `-o offset=N` (bytes).
- **FUSE library**: libfuse3 high-level API.
- **Targets**: Linux is primary; macOS via macFUSE is best-effort.

## Build prerequisites

**Linux:**
```
sudo apt install libfuse3-dev pkg-config build-essential
```

**macOS (best-effort):**
```
brew install macfuse pkg-config
```

---

## Phase 0 - Project scaffolding [DONE]
- [x] Create `src/`, `ods2lib/`, `test/`, `build/`
- [x] Drop the read-only subset of the upstream ODS-2 sources into `ods2lib/`
- [x] Stub `phyvirt.h` with the API used by `access.c`
- [x] Write a portable `Makefile` (Linux: `pkg-config fuse3`; macOS: `pkg-config fuse` via macFUSE)
- [x] Verify `ods2lib/` builds on its own as `libods2.a`
- [x] Stub `compat_glue.c` with a minimal `printmsg` and write-side stoppers

## Phase 1 - phyfuse backend + mount bring-up [DONE]
- [x] `src/phyfuse.c`: implement `virt_open`/`virt_read`/`virt_close` on top of `pread()` with offset
- [x] `src/fuse_ods2.c`: argv / `-o` parsing (image path, mountpoint, options)
- [x] Wire `mount(...)` at FUSE startup; `dismount(...)` at shutdown
- [x] Print HOME diagnostics (volume name, size, free clusters) under `-o debug`
- [ ] Smoke: `fuse-ods2 -d image.dsk /tmp/m` mounts cleanly, mountpoint is empty until phase 2  *(pending: requires libfuse3, deferred to Linux validation)*

## Phase 2 - getattr + readdir [CODE DONE, RUNTIME TBD]
- [x] `src/lookup.c`: POSIX path -> FID via `direct_dirid` + manual dir-block scan for the file part
- [ ] LRU cache (path -> FID), 1024 entries  *(deferred: direct_dirid already memoises directories)*
- [x] `ods2_getattr`: populate `struct stat` (size, blocks, mtime, ctime, mode, uid, gid)
- [x] VMSTIME -> `time_t` conversion (custom 100ns-since-1858 calc; vmstime.c kept for any future need)
- [x] `fh2$w_fileprot` -> `mode_t` (RWE for owner/group/world; drop D, drop "system" by folding into owner)
- [x] `ods2_readdir`: iterate directory blocks, parse `dir$r_rec` / `dir$r_ent`, handle split-record duplicates
- [x] Latest-version filter by default; `-o allversions` exposes everything as `;n`
- [x] `-o lower`: lowercase the exposed names
- [ ] Test: `ls -l /tmp/m` shows a coherent MFD  *(deferred to Linux validation)*

## Phase 3 - open + read (raw) [CODE DONE, RUNTIME TBD]
- [x] `ods2_open`: `accessfile`, store FCB in `fi->fh`, refuse write flags
- [x] `ods2_read`: loop `accesschunk` from `vbn = offset/512 + 1`, splice partial first/last bytes
- [x] `ods2_release`: `deaccessfile`
- [x] Honour EOF block / `fat$w_ffbyte`
- [ ] Honour highwater (return zeros past `fcb->highwater`)  *(deferred: most images are dense)*
- [ ] Test: `cmp` STREAM_LF and FIX files against an ods2 extraction  *(deferred to Linux validation)*

## Phase 4 - text-mode (`-o textmode`) [CODE DONE, RUNTIME TBD]
- [x] `src/recfmt.c`: full-file decoder with per-FID buffer cache
- [x] Per-record-format strategies (FIX/VAR/VFC/STM/STMLF/STMCR + UDF passthrough)
- [x] Recompute `st_size` in `getattr` when textmode (cache the logical length)
- [x] Cache cleared on dismount
- [ ] Test on `.LIS` / `.TXT` files in VAR format  *(deferred to phase 7 e2e)*

## Phase 5 - offset + volume set [CODE DONE, RUNTIME TBD]
- [x] `-o offset=N`: implemented in phyfuse, exposed on the CLI
- [x] `-o vol=path1,path2,...`: pass `devices[]` to `ods2_mount()` for multi-RVN volumes (with bounded list, leak-free parser)
- [ ] Offset test: prepend an MBR (`cat mbr raw > padded`) and verify mount with offset  *(deferred to Linux)*
- [ ] Volume set test if such an image can be generated upstream  *(deferred to Linux)*

## Phase 6 - hardening [MOSTLY DONE]
- [ ] Global mutex around ods2_mount/dismount/access (re-enable multithreaded FUSE later)  *(MVP runs single-threaded per -s)*
- [x] Clean dismount on SIGINT/SIGTERM (fuse_main breaks the loop; do_dismount runs after)
- [x] Sensible `statfs` (block size, total blocks; free blocks pinned to 0 for RO)
- [x] `readlink` -> `EINVAL` (ODS-2 has no symlinks)
- [x] Every write op returns `EROFS` (`mknod`, `mkdir`, `unlink`, `rmdir`, `symlink`, `rename`, `link`, `chmod`, `chown`, `truncate`, `write`, `setxattr`, `removexattr`, `create`, `utimens`, `fallocate`, `copy_file_range`)

## Phase 7 - tests and CI
- [ ] `test/make_image.sh`: produce a reproducible image (STREAM_LF, FIX, VAR files; nested dirs; multiple versions)
- [ ] `test/smoke.sh`: ls / find / cat / cmp on the known fileset
- [ ] (Stretch) GitHub Actions workflow for Linux build + smoke

## Phase 8 - macOS portability (parallel)
- [ ] Detect platform in `Makefile` (Linux -> `pkg-config fuse3`; macOS -> `pkg-config fuse` via macFUSE)
- [ ] Portable endian wrapper (`<endian.h>` on Linux, `<machine/endian.h>` on macOS)
- [ ] Manual smoke test on macOS with macFUSE installed
- [ ] Document macOS quirks in the README (kext approval, performance)
