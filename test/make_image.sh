#!/usr/bin/env bash
# Build the upstream ods2 utility and use it to create a small,
# reproducible ODS-2 test image with files of known content.
#
# Output (in $OUT_DIR, default ./test/_out):
#   test.dsk           the ODS-2 image (RM05 medium, label TESTVOL)
#   src/HELLO.TXT      original byte-for-byte copy of the canned files
#   src/LINES.TXT
#   src/SUB/INFO.TXT
#   ods2               the upstream binary (kept for the smoke driver)

set -euo pipefail

ROOT="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"
OUT_DIR="${OUT_DIR:-$ROOT/test/_out}"
SIM_DIR="$OUT_DIR/simtools"

mkdir -p "$OUT_DIR/src/SUB"

# 1) Canned source files.  Plain ASCII so the ods2 COPY /ASCII path is
# happy and the byte-level comparison is trivial.
cat > "$OUT_DIR/src/HELLO.TXT" <<'EOF'
Hello from ODS-2
EOF
printf 'line one\nline two\nline three\n' > "$OUT_DIR/src/LINES.TXT"
printf 'nested file\n' > "$OUT_DIR/src/SUB/INFO.TXT"

# 2) Upstream ods2 (one-shot, depth-1 clone).
if [ ! -x "$OUT_DIR/ods2" ]; then
    rm -rf "$SIM_DIR"
    git clone --depth=1 https://github.com/open-simh/simtools "$SIM_DIR"
    (
        cd "$SIM_DIR/extracters/ods2"
        ln -sf makefile.unix Makefile
        make -j"$(nproc 2>/dev/null || echo 2)"
    )
    cp "$SIM_DIR/extracters/ods2/ods2"   "$OUT_DIR/ods2"
    # ods2 looks up its help / message catalogues next to the binary.
    cp -f "$SIM_DIR/extracters/ods2/"*.hlb "$OUT_DIR/" 2>/dev/null || true
    cp -f "$SIM_DIR/extracters/ods2/"*.mdf "$OUT_DIR/" 2>/dev/null || true
fi

ODS2="$OUT_DIR/ods2"

# 3) Drive ods2 with a command file.  ods2 parses '/' as the start of
# a DCL qualifier, so any Unix path with slashes must be wrapped in
# double quotes ("/path/to/file").  Easiest: cd into OUT_DIR and use
# relative paths with no leading slashes.  HELLO.TXT is staged via the
# parent path "src/HELLO.TXT" which still contains a '/', so we quote
# those too.
# INITIALIZE auto-mounts the freshly created image, so we skip the
# explicit MOUNT.  ods2 prompts for confirmation on destructive ops;
# we feed 'yes\n' on stdin so it behaves non-interactively.
cat > "$OUT_DIR/build.com" <<'EOF'
INITIALIZE /MEDIUM:RM05 /LOG test.dsk TESTVOL
COPY /TO_FILES-11/ASCII /LOG "src/HELLO.TXT" [000000]HELLO.TXT
COPY /TO_FILES-11/ASCII /LOG "src/LINES.TXT" [000000]LINES.TXT
CREATE /DIRECTORY /LOG [SUB]
COPY /TO_FILES-11/ASCII /LOG "src/SUB/INFO.TXT" [SUB]INFO.TXT
COPY /TO_FILES-11/ASCII /LOG "src/HELLO.TXT" [000000]HELLO.TXT
COPY /TO_FILES-11/ASCII /LOG "src/HELLO.TXT" [000000]HELLO.TXT
DIRECTORY [000000]
DIRECTORY [SUB]
DISMOUNT A:
EXIT
EOF

rm -f "$OUT_DIR/test.dsk"
# A handful of 'y' answers; only one prompt is expected (INITIALIZE),
# but we leave headroom in case future commands grow extra prompts.
# Using printf instead of yes(1) avoids SIGPIPE/141 under pipefail.
( cd "$OUT_DIR" && printf 'y\ny\ny\ny\ny\ny\ny\ny\n' | "$ODS2" "@build.com" )

echo "made: $OUT_DIR/test.dsk"
ls -lah "$OUT_DIR/test.dsk"
