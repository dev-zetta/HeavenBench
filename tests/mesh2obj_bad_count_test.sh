#!/bin/sh
#
# Tool-level test for the mesh2obj per-surface count guard.
#
# mesh2obj is a standalone CLI tool (not part of the benchmark test harness),
# so this is a self-contained, dependency-free shell harness: it crafts mi08
# buffers with printf, invokes the built mesh2obj binary on them, and asserts
# the documented behaviour.
#
#   1. A crafted mi08 with an out-of-range per-surface vertex count must exit
#      non-zero, must not crash (no signal), and must not write an OBJ.
#   2. A well-formed (empty-surface) mi08 must exit zero and write an OBJ.
#
# Usage: mesh2obj_bad_count_test.sh <path-to-mesh2obj-binary>

set -u

MESH2OBJ="${1:?usage: $0 <path-to-mesh2obj-binary>}"

if [ ! -x "$MESH2OBJ" ]; then
    echo "FAIL: mesh2obj binary not found or not executable: $MESH2OBJ" >&2
    exit 1
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

# --- mi08 byte-emitting helpers (portable octal printf) ---------------------

# 4-byte little-endian magic "mi08".
emit_magic() {
    printf '\155\151\060\070'
}

# Emit N zero bytes (used for the 40-byte bounding-volume blocks).
emit_zeros() {
    head -c "$1" /dev/zero
}

# int32 little-endian: 1
emit_i32_one() {
    printf '\001\000\000\000'
}

# int32 little-endian: 0
emit_i32_zero() {
    printf '\000\000\000\000'
}

# Surface header for a surface named "abc": int32 name_len=4, "abc\0",
# then the 40-byte surface bounding volume.
emit_surface_header_abc() {
    printf '\004\000\000\000'   # name_len = 4
    printf 'abc\000'            # "abc" + null terminator
    emit_zeros 40               # surface bounding volume (10 floats)
}

# ---------------------------------------------------------------------------
# Case 1: crafted bad-count buffer -> non-zero exit, no crash, no OBJ.
# ---------------------------------------------------------------------------
BAD="$WORKDIR/bad.mesh"
{
    emit_magic                 # "mi08"
    emit_zeros 40              # mesh bounding volume (10 floats)
    emit_i32_one               # num_surfaces = 1
    emit_surface_header_abc    # one surface header
    # Per-surface data: an out-of-range vertex count (0x7FFFFFFF = 2147483647,
    # well above the tool's 10,000,000 element bound).
    printf '\377\377\377\177'  # num_verts = 2147483647
} > "$BAD"

"$MESH2OBJ" "$BAD" > "$WORKDIR/bad.out" 2> "$WORKDIR/bad.err"
status=$?

# Exit status >= 128 indicates death by signal (crash); reject that explicitly.
if [ "$status" -ge 128 ]; then
    cat "$WORKDIR/bad.err" >&2
    fail "mesh2obj crashed on bad-count input (signal exit status $status)"
fi

if [ "$status" -eq 0 ]; then
    fail "mesh2obj returned 0 on bad-count input; expected non-zero"
fi

if [ -e "$WORKDIR/bad.obj" ]; then
    fail "mesh2obj wrote an OBJ for bad-count input ($WORKDIR/bad.obj)"
fi

echo "PASS: bad-count input -> exit $status, no crash, no OBJ written"

# ---------------------------------------------------------------------------
# Case 2 (sanity): well-formed empty-surface buffer -> zero exit, OBJ written.
# ---------------------------------------------------------------------------
GOOD="$WORKDIR/good.mesh"
{
    emit_magic                 # "mi08"
    emit_zeros 40              # mesh bounding volume
    emit_i32_one               # num_surfaces = 1
    emit_surface_header_abc    # one surface header
    emit_i32_zero              # num_verts = 0
    emit_i32_zero              # num_tc0   = 0
    emit_i32_zero              # num_tc1   = 0
    emit_i32_zero              # num_tris  = 0
} > "$GOOD"

"$MESH2OBJ" "$GOOD" > "$WORKDIR/good.out" 2> "$WORKDIR/good.err"
status=$?

if [ "$status" -ne 0 ]; then
    cat "$WORKDIR/good.err" >&2
    fail "mesh2obj returned $status on well-formed input; expected 0"
fi

if [ ! -e "$WORKDIR/good.obj" ]; then
    fail "mesh2obj did not write an OBJ for well-formed input"
fi

echo "PASS: well-formed input -> exit 0, OBJ written"

echo "All mesh2obj bad-count tests passed."
exit 0
