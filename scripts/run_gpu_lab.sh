#!/usr/bin/env bash
# Optional native Linux compute diagnostics; scene settings/scores are untouched.
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." &>/dev/null && pwd)"
source "$SCRIPT_DIR/build_paths.sh"
build_dir="${HEAVEN_BUILD_DIR:-$(heaven_renderer_build_dir linux Release arm64-v8a "${HEAVEN_COMPILER:-clang}")}"
do_build=0
arguments=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build) do_build=1; shift ;;
        --build-dir)
            [[ $# -gt 1 && -n "$2" ]] || { echo 'Error: --build-dir needs a directory' >&2; exit 2; }
            build_dir="$2"; shift 2 ;;
        *) arguments+=("$1"); shift ;;
    esac
done
if (( do_build )); then
    "$SCRIPT_DIR/build.sh" --build-dir "$build_dir" \
        --cmake-arg -DHEAVEN_BUILD_GPU_LAB=ON --target heaven_gpu_lab
fi
executable="$build_dir/gpu_lab/heaven_gpu_lab"
if [[ ! -x "$executable" ]]; then
    echo 'GPU lab is optional. Build it with scripts/run_gpu_lab.sh --build --list' >&2
    exit 1
fi
exec "$executable" "${arguments[@]}"
