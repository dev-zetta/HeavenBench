#!/bin/sh

set -u

UNIGINEX="${1:?usage: $0 <path-to-uniginex-binary>}"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

printf 'not an archive' > "$WORKDIR/invalid.ung"
"$UNIGINEX" "$WORKDIR/invalid.ung" "$WORKDIR/out" \
    > "$WORKDIR/stdout" 2> "$WORKDIR/stderr"
status=$?

if [ "$status" -eq 0 ] || [ "$status" -ge 128 ]; then
    cat "$WORKDIR/stdout" "$WORKDIR/stderr" >&2
    fail "invalid UNG signature did not produce a clean non-zero exit"
fi

if ! grep -q "invalid signature" "$WORKDIR/stderr"; then
    cat "$WORKDIR/stdout" "$WORKDIR/stderr" >&2
    fail "invalid UNG signature diagnostic is missing"
fi

if [ -e "$WORKDIR/out" ] && find "$WORKDIR/out" -type f | grep -q .; then
    fail "invalid UNG input produced output files"
fi

echo "PASS: invalid UNG signature is rejected without output"
