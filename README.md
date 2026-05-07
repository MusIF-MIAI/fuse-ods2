# fuse-ods2

[![build](https://github.com/MusIF-MIAI/fuse-ods2/actions/workflows/build.yml/badge.svg)](https://github.com/MusIF-MIAI/fuse-ods2/actions/workflows/build.yml)

Read-only FUSE driver for **ODS-2** (Files-11 On-Disk Structure Level 2),
the native filesystem of OpenVMS.

`fuse-ods2` lets you mount raw disk dumps of ODS-2 volumes on Linux (and,
best-effort, on macOS via macFUSE) so you can browse and copy files with
ordinary tools (`ls`, `cat`, `cp`, `find`, ...) without booting an
emulator or running an interactive ODS-2 utility.

## Status

Early development. See [`TODO.md`](TODO.md) for the roadmap.

## Building

### Linux (Debian / Ubuntu)

```sh
sudo apt install libfuse3-dev pkg-config build-essential
make
```

CI builds on `debian:stable`.  Fedora / Arch users want
`fuse3-devel` / `fuse3` instead.

### macOS (best-effort)

```sh
brew install --cask macfuse
brew install pkg-config
make
```

The Makefile detects Darwin and resolves `pkg-config fuse3` if macFUSE
exposes that name, or falls back to `pkg-config fuse`.

Caveats on macOS:

- macFUSE installs a kernel extension that requires user approval in
  *System Settings -> Privacy & Security* the first time it loads.
- File ownership: macFUSE refuses to expose a mount to anyone other
  than the mounter unless you pass `-o allow_other` (and macFUSE has
  it enabled).
- Linux remains the primary tested platform; the upstream ods2 build
  used by the smoke test is not exercised in CI on macOS.

## Usage

```sh
fuse-ods2 [options] <image> <mountpoint>
```

Mount a raw image read-only:

```sh
mkdir /tmp/m
fuse-ods2 vms-disk.dsk /tmp/m
ls /tmp/m
fusermount3 -u /tmp/m
```

### Options

| Option              | Effect                                                            |
|---------------------|-------------------------------------------------------------------|
| `-o offset=N`       | Skip N bytes at the start of the image (e.g. an MBR prefix)       |
| `-o vol=A,B,...`    | Volume set: extra image files for RVNs 2..N                       |
| `-o allversions`    | Show every file version as `NAME.EXT;n` (default: latest only)    |
| `-o lower`          | Lowercase file names (default: uppercase, as on disk)             |
| `-o textmode`       | Strip RMS record headers from VAR/VFC files and convert to LF     |
| `-o uid=N`/`gid=N`  | Force a specific uid/gid on every file                            |
| `-o debug`          | Verbose diagnostics on stderr                                     |
| `-s`                | Single-threaded (recommended at this stage)                       |

The filesystem is always mounted read-only at the operation layer:
every `mknod` / `mkdir` / `unlink` / `write` / `chmod` / `setxattr` /
`fallocate` / ... returns `EROFS`.  If you also want the kernel-level
`ro` flag (so even root sees `Read-only file system`), pass `-o ro` on
the command line.

`statfs` reports the volume's total block count derived from the
home-block cluster size and image size.  Free-block accounting needs
the storage bitmap (a write-side concern) and is reported as zero.

## `catvms` — decoding extracted record files

VMS text files are usually stored as RMS records, not as a flat byte
stream.  `fuse-ods2 -o textmode` decodes them on the fly while the
volume is mounted, but if you have already copied a file out in raw
mode (the default), `catvms` does the same job offline:

```sh
catvms file.txt              # autodetect, falls back to VAR
catvms --var      < f.txt    # force VAR (length-prefixed records)
catvms --vfc=2    f.lis      # VFC, 2-byte fixed control header
catvms --fix=80   card.dat   # FIX records, 80 bytes each
catvms --stmlf    f.txt      # already a stream, pass through
catvms --stmcr    f.mac      # CR-delimited stream, translate to LF
catvms --no-lf    --var f    # do not append LF after each record
```

`catvms` has no library dependencies; build it via `make` (or
`make catvms`) along with `fuse-ods2`.

## Project layout

```
src/        Wrapper code: FUSE operations, path lookup, I/O backend
ods2lib/    ODS-2 read-only core (extracted subset of the open-simh
            "ods2" utility, unmodified, original notices preserved)
test/       Test image generators and smoke scripts
build/      Object files (created by make)
```

## Limitations

- Read-only.
- Indexed RMS files are exposed as raw bytes (no record-level access).
- ACLs and VMS-specific protection bits are mapped down to POSIX
  `rwx` bits, dropping the *delete* bit (which has no POSIX equivalent).
- ODS-5 volumes are not supported.
- VHD container images are not supported (raw images only).

## License

MIT, see [`LICENSE`](LICENSE).

The contents of `ods2lib/` are derivative work from the open-simh
`simtools/extracters/ods2` package and retain the original authors'
notices and license terms.
