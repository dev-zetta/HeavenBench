#!/usr/bin/env bash
# Shared local/CI Android pipeline, run inside the Docker Android build stage.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
# shellcheck source=build_paths.sh
source "$SCRIPT_DIR/build_paths.sh"
HEAVEN_BUILD_ROOT="$(realpath -m "$HEAVEN_BUILD_ROOT")"
export HEAVEN_BUILD_ROOT

ABI=arm64-v8a
JOBS="${HEAVEN_JOBS:-2}"
OUTPUT_DIR=/artifacts
usage() {
    echo 'Usage: scripts/ci_android.sh [--abi arm64-v8a|x86_64] [--jobs N] [--output-dir DIR]'
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        --abi|--jobs|--output-dir)
            [[ $# -ge 2 && -n "$2" ]] || { echo "Missing value for $1" >&2; exit 2; }
            case "$1" in
                --abi) ABI="$2" ;;
                --jobs) JOBS="$2" ;;
                --output-dir) OUTPUT_DIR="$2" ;;
            esac
            shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done
[[ "$ABI" == arm64-v8a || "$ABI" == x86_64 ]] || { echo "Unsupported ABI: $ABI" >&2; exit 2; }
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo '--jobs must be a positive integer' >&2; exit 2; }
OUTPUT_DIR="$(realpath -m "$OUTPUT_DIR")"
mkdir -p "$OUTPUT_DIR/logs"
trap 'exit_status=$?; printf "%s\n" "$exit_status" > "$OUTPUT_DIR/logs/android-exit-code.txt"; exit "$exit_status"' EXIT

# Preserve lint evidence even when the build fails. Disable errexit only around
# the pipeline so the helper's status, not tee's success, controls the result.
set +e
"$SCRIPT_DIR/build_android.sh" --abi "$ABI" --jobs "$JOBS" \
    2>&1 | tee "$OUTPUT_DIR/logs/android-build.log"
pipeline_status=("${PIPESTATUS[@]}")
set -e
build_status="${pipeline_status[0]}"
if [[ "$build_status" -eq 0 ]]; then build_status="${pipeline_status[1]}"; fi

shopt -s nullglob
reports=("$HEAVEN_BUILD_ROOT/android-gradle/$ABI/app/reports/"lint-results-*)
copy_status=0
if [[ ${#reports[@]} -gt 0 ]]; then
    cp -a "${reports[@]}" "$OUTPUT_DIR/logs/" || copy_status=$?
fi
if [[ -f /opt/android-toolchain.json ]]; then
    cp /opt/android-toolchain.json "$OUTPUT_DIR/logs/" || copy_status=$?
fi
if [[ "$build_status" -eq 0 ]]; then build_status="$copy_status"; fi
[[ "$build_status" -eq 0 ]] || exit "$build_status"

VERSION="$(python3 - "$REPO_ROOT/benchmark/CMakeLists.txt" <<'PY'
from pathlib import Path
import re
import sys
print(re.search(r'project\(HeavenBench VERSION ([0-9.]+)', Path(sys.argv[1]).read_text())[1])
PY
)"
APK_NAME="HeavenBench-$VERSION-android-$ABI-debug.apk"
PACKAGE_DIR="$HEAVEN_BUILD_ROOT/packages"
[[ -f "$PACKAGE_DIR/$APK_NAME" && -f "$PACKAGE_DIR/$APK_NAME.sha256" ]] || {
    echo "Android helper did not produce the expected APK/checksum: $APK_NAME" >&2
    exit 1
}
(cd "$PACKAGE_DIR" && sha256sum --check "$APK_NAME.sha256")
if [[ "$OUTPUT_DIR" != "$PACKAGE_DIR" ]]; then
    cp "$PACKAGE_DIR/$APK_NAME" "$PACKAGE_DIR/$APK_NAME.sha256" "$OUTPUT_DIR/"
fi
printf '%s\n' "Verified Android artifact: $OUTPUT_DIR/$APK_NAME"
