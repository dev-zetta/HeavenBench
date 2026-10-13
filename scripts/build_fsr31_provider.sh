#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${HEAVEN_FSR31_SOURCE:-$root/build/dependencies/fidelityfx-sdk-1.1.4}"
build_dir="${HEAVEN_FSR31_BUILD:-$root/build/fsr31-provider}"
if [[ ! -f "$source_dir/sdk/src/backends/vk/ffx_vk.cpp" ]]; then
    python3 "$root/scripts/fetch_fsr31.py" --output "$source_dir"
fi
cmake -S "$root/benchmark/src/rhi/frame_generation" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="${HEAVEN_C_COMPILER:-clang}" \
    -DCMAKE_CXX_COMPILER="${HEAVEN_CXX_COMPILER:-clang++}" \
    -DCMAKE_CXX_FLAGS="${HEAVEN_FSR31_CXX_FLAGS:--stdlib=libc++}" \
    -DHEAVEN_FSR31_SOURCE="$source_dir" "${@}"
cmake --build "$build_dir" --parallel "${HEAVEN_FSR31_JOBS:-4}"
