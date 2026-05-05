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
brew install macfuse pkg-config
make
```

macFUSE requires user approval the first time the kernel extension is
loaded.

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

The filesystem is always mounted read-only; any write attempt returns
`EROFS`.

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
