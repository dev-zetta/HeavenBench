#!/usr/bin/env bash
# Build and package an Apple Silicon app. Original Heaven assets stay external.
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
source "$SCRIPT_DIR/build_paths.sh"
BUILD_DIR="${HEAVEN_BUILD_DIR:-}"
OUTPUT_DIR="$HEAVEN_BUILD_ROOT/packages/macos"
JOBS=2
SKIP_BUILD=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir|--output-dir|--jobs)
            [[ $# -ge 2 && -n "$2" ]] || { echo "$1 requires a value" >&2; exit 2; } ;;
    esac
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --help|-h)
            cat <<'EOF'
Usage: scripts/package_macos.sh [--build-dir DIR] [--output-dir DIR]
       [--jobs N] [--skip-build]

Builds native arm64 Release binaries and creates HeavenBench-VERSION-macos-arm64.zip
with a SHA-256 sidecar. Requires macOS 14+, Xcode tools and the Homebrew build
dependencies documented in docs/BUILDING.md. Bundles Vulkan/MoltenVK, SDL and
LLVM runtime libraries, redistributable resources and notices. The app is ad-hoc
signed; it has no Developer ID signature or Apple notarization.
EOF
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
[[ $(uname -s) == Darwin && $(uname -m) == arm64 ]] || { echo 'macOS packaging requires a native Apple Silicon host' >&2; exit 1; }
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo '--jobs must be positive' >&2; exit 2; }
BUILD_DIR="${BUILD_DIR:-$(heaven_renderer_build_dir macos Release)}"
if [[ "$SKIP_BUILD" == 0 ]]; then
    "$SCRIPT_DIR/build.sh" --platform macos --release --build-dir "$BUILD_DIR" --jobs "$JOBS" \
        --target heaven_benchmark --target heaven_asset_packer
    python3 "$SCRIPT_DIR/bundle_macos.py" tools --build-dir "$BUILD_DIR" --jobs "$JOBS"
fi
python3 "$SCRIPT_DIR/bundle_macos.py" package --build-dir "$BUILD_DIR" --output-dir "$OUTPUT_DIR"
