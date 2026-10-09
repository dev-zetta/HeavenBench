#!/usr/bin/env bash
# Shared build-directory conventions for HeavenBench scripts.
#
# Callers must define REPO_ROOT before sourcing this file.

if [[ -z "${REPO_ROOT:-}" ]]; then
    echo "Error: REPO_ROOT must be set before sourcing build_paths.sh" >&2
    return 1
fi

HEAVEN_BUILD_ROOT="${HEAVEN_BUILD_ROOT:-$REPO_ROOT/build}"
HEAVEN_AOCC_ROOT="${HEAVEN_AOCC_ROOT:-$HEAVEN_BUILD_ROOT/toolchains/aocc-5.2.0}"

heaven_renderer_build_dir() {
    local platform="${1:-linux}"
    local build_type="${2:-Release}"
    local android_abi="${3:-arm64-v8a}"
    local compiler="${4:-clang}"
    local directory="$HEAVEN_BUILD_ROOT/renderer"

    case "$platform" in
        linux)
            if [[ "$compiler" == "aocc" ]]; then
                directory+="-aocc"
            fi ;;
        windows)
            directory+="-windows" ;;
        android)
            directory+="-android-$android_abi" ;;
        *)
            echo "Error: unsupported platform '$platform'" >&2
            return 1 ;;
    esac

    if [[ "$build_type" == "Debug" ]]; then
        directory+="-debug"
    fi

    printf '%s\n' "$directory"
}
