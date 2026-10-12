#!/usr/bin/env bash
# Shared native macOS build/test/package policy for local use and GitHub Actions.
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
source "$SCRIPT_DIR/build_paths.sh"
BUILD_DIR="${HEAVEN_BUILD_DIR:-}"
OUTPUT_DIR="$HEAVEN_BUILD_ROOT/packages/macos"
JOBS=2
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir|--output-dir|--jobs)
            [[ $# -ge 2 && -n "$2" ]] || { echo "$1 requires a value" >&2; exit 2; } ;;
    esac
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        --help|-h)
            echo 'Usage: scripts/ci_macos.sh [--build-dir DIR] [--output-dir DIR] [--jobs N]'
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
[[ $(uname -s) == Darwin && $(uname -m) == arm64 ]] || { echo 'macOS CI requires a native Apple Silicon host' >&2; exit 1; }
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo '--jobs must be positive' >&2; exit 2; }
BUILD_DIR="${BUILD_DIR:-$(heaven_renderer_build_dir macos Release)}"
mkdir -p "$OUTPUT_DIR/logs"
exec > >(tee "$OUTPUT_DIR/logs/macos-build.log") 2>&1
"$SCRIPT_DIR/fetch_dependencies.sh"
"$SCRIPT_DIR/build.sh" --platform macos --release --build-dir "$BUILD_DIR" --jobs "$JOBS" \
    --target heaven_benchmark --target heaven_asset_packer --target heaven_benchmark_tests \
    --target heaven_profiler_trace_tests --target heaven_benchmark_smoke
python3 "$SCRIPT_DIR/bundle_macos.py" tools --build-dir "$BUILD_DIR" --jobs "$JOBS"
# Discard caller asset overrides; these tests exercise their own fixtures.
unset HEAVEN_RESOURCE_DIR HEAVEN_DATA HEAVEN_DEFAULT_DATA HEAVEN_USER_DIR
RC_PARAMS='seed=20260930' ctest --test-dir "$BUILD_DIR" --output-on-failure --no-tests=error \
    --timeout 300 --tests-regex '^heaven_(benchmark_tests|profiler_trace_tests|microprofile_analysis|runtime_paths_cli|asset_packer_cli|gtao_bent_normal_contract|specular_occluded_moment_contract|surface_color_contract)$'
ctest --test-dir "$BUILD_DIR/tools" --output-on-failure --no-tests=error --timeout 120
# Default CI does not establish MoltenVK GPU/display acceptance on hosted runners.
HEAVEN_SMOKE=0 ctest --test-dir "$BUILD_DIR" --output-on-failure --no-tests=error \
    --timeout 30 --tests-regex '^heaven_benchmark_smoke$'
"$SCRIPT_DIR/package_macos.sh" --skip-build --build-dir "$BUILD_DIR" --output-dir "$OUTPUT_DIR"
VERSION=$(python3 "$SCRIPT_DIR/bundle_macos.py" version)
python3 "$SCRIPT_DIR/verify_macos.py" "$OUTPUT_DIR/HeavenBench-$VERSION-macos-arm64.zip" \
    --receipt "$OUTPUT_DIR/macos-package-verification.json"
