#!/usr/bin/env bash
# End-to-end smoke test: mount the image produced by make_image.sh
# with fuse-ods2 and check that ls/cat produce the expected content.

set -euo pipefail

ROOT="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"
OUT_DIR="${OUT_DIR:-$ROOT/test/_out}"
IMG="${IMG:-$OUT_DIR/test.dsk}"
BIN="${BIN:-$ROOT/fuse-ods2}"
MNT="$(mktemp -d)"

if [ ! -f "$IMG" ]; then
    echo "smoke: image '$IMG' is missing - run make_image.sh first" >&2
    exit 2
fi
if [ ! -x "$BIN" ]; then
    echo "smoke: binary '$BIN' is missing - run 'make bin' first" >&2
    exit 2
fi

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    rmdir "$MNT" 2>/dev/null || true
}
trap cleanup EXIT

fail() {
    echo "smoke: FAIL - $*" >&2
    exit 1
}

ok() {
    echo "smoke: ok  - $*"
}

# ---- mount (single-threaded, foreground in background) ----
"$BIN" -s -f "$IMG" "$MNT" &
FPID=$!
# Give the kernel a moment to register the mount.
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if mountpoint -q "$MNT"; then break; fi
    sleep 0.2
done
if ! mountpoint -q "$MNT"; then
    fail "mountpoint did not appear (pid=$FPID)"
fi
ok "mounted at $MNT"

# ---- listing root ----
echo "smoke: --- ls -la $MNT ---"
ls -la "$MNT" || true
echo "smoke: --- end ---"

listing="$(ls "$MNT")"
echo "$listing" | grep -q '^HELLO\.TXT$'  || fail "HELLO.TXT missing in root listing (got: $listing)"
echo "$listing" | grep -q '^LINES\.TXT$'  || fail "LINES.TXT missing in root listing (got: $listing)"
echo "$listing" | grep -q '^SUB$'         || fail "SUB directory missing in root listing (got: $listing)"
ok "root listing contains the expected entries"

# ---- file content ----
grep -q 'Hello from ODS-2' "$MNT/HELLO.TXT" \
    || fail "HELLO.TXT does not contain expected greeting"
ok "HELLO.TXT content looks right"

# ---- subdir traversal ----
sub_listing="$(ls "$MNT/SUB")"
echo "$sub_listing" | grep -q '^INFO\.TXT$' \
    || fail "SUB/INFO.TXT missing in subdir listing"
ok "subdir traversal works"

# ---- read-only enforcement ----
if : > "$MNT/SHOULD_FAIL.TXT" 2>/dev/null; then
    fail "creating a file in the mount succeeded; expected EROFS"
fi
ok "write attempt is rejected as expected"

# ---- versioned file (HELLO.TXT was copied 3 times -> versions 1..3) ----
fusermount3 -u "$MNT"
wait "$FPID" || true
"$BIN" -s -o allversions "$IMG" "$MNT"
for _ in 1 2 3 4 5; do
    if mountpoint -q "$MNT"; then break; fi
    sleep 0.2
done
versions="$(ls "$MNT" | grep -E '^HELLO\.TXT;[0-9]+$' || true)"
nvers="$(printf '%s\n' "$versions" | grep -c . || true)"
[ "$nvers" -ge 3 ] \
    || fail "expected >=3 versions of HELLO.TXT under -o allversions, got: '$versions'"
ok "-o allversions exposes $nvers versions of HELLO.TXT"

echo "smoke: PASSED"
