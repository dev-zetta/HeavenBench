#!/usr/bin/env bash
# Build the standalone ZIP packer without configuring graphics dependencies.
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
source "$SCRIPT_DIR/build_paths.sh"
BUILD_DIR="$HEAVEN_BUILD_ROOT/asset-tools"
JOBS=2
BUILD_TYPE=Release
CONFIGURE_ONLY=0
C_COMPILER="${HEAVEN_C_COMPILER:-clang}"
CXX_COMPILER="${HEAVEN_CXX_COMPILER:-clang++}"
EXTRA_CMAKE_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir|--jobs|--cmake-arg)
            [[ $# -ge 2 && -n "$2" ]] || { echo "$1 requires a value" >&2; exit 1; } ;;
    esac
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        --debug) BUILD_TYPE=Debug; shift ;;
        --configure-only) CONFIGURE_ONLY=1; shift ;;
        --cmake-arg) EXTRA_CMAKE_ARGS+=("$2"); shift 2 ;;
        --help|-h)
            cat <<'EOF'
Usage: scripts/build_asset_tools.sh [--build-dir DIR] [--jobs N] [--debug] [--configure-only]
       [--cmake-arg ARG]

Builds heaven_asset_packer under build/asset-tools with Clang and libc++.
Requires Linux, CMake, Ninja, zlib/GLM headers, and pinned CPU dependencies
from scripts/fetch_dependencies.sh. Does not configure SDL, Vulkan, or Filament.
HEAVEN_BUILD_ROOT and HEAVEN_C_COMPILER/HEAVEN_CXX_COMPILER are supported.
EOF
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "--jobs must be positive" >&2; exit 1; }
[[ $(uname -s) == Linux ]] || { echo "Use build_windows.ps1 for native Windows tools." >&2; exit 1; }
for tool in cmake ninja "$C_COMPILER" "$CXX_COMPILER"; do
    command -v "$tool" >/dev/null || { echo "Required tool not found: $tool" >&2; exit 1; }
done
cmake -S "$REPO_ROOT/tools/asset_packer" -B "$BUILD_DIR" -G Ninja \
    "-DCMAKE_BUILD_TYPE=$BUILD_TYPE" "-DCMAKE_C_COMPILER=$C_COMPILER" \
    "-DCMAKE_CXX_COMPILER=$CXX_COMPILER" '-DCMAKE_CXX_FLAGS=-stdlib=libc++' \
    "${EXTRA_CMAKE_ARGS[@]}"
if [[ "$CONFIGURE_ONLY" == 0 ]]; then
    cmake --build "$BUILD_DIR" --target heaven_asset_packer --parallel "$JOBS"
fi
