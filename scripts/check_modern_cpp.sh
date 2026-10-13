#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=build_paths.sh
source "${REPO_ROOT}/scripts/build_paths.sh"
tools_build="${HEAVEN_TOOLS_BUILD_DIR:-${HEAVEN_BUILD_ROOT}/tools}"
renderer_build="${HEAVEN_BUILD_DIR:-$(heaven_renderer_build_dir)}"
jobs="${HEAVEN_TIDY_JOBS:-2}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --tools-build-dir|--renderer-build-dir|--jobs|-j)
            if [[ $# -lt 2 || -z "$2" ]]; then
                echo "error: $1 requires a value" >&2
                exit 1
            fi
            case "$1" in
                --tools-build-dir) tools_build="$2" ;;
                --renderer-build-dir) renderer_build="$2" ;;
                --jobs|-j) jobs="$2" ;;
            esac
            shift 2 ;;
        --help|-h)
            cat <<EOF
Usage: $0 [--tools-build-dir DIR] [--renderer-build-dir DIR] [--jobs N]

Audit project-owned C++ using existing compilation databases. Parallel jobs
default to 2. HEAVEN_BUILD_ROOT, HEAVEN_TOOLS_BUILD_DIR, HEAVEN_BUILD_DIR and
HEAVEN_TIDY_JOBS override the defaults. HEAVEN_RUN_CLANG_TIDY and
HEAVEN_CLANG_TIDY select explicit executables; versioned LLVM tools are also
discovered automatically when unversioned commands are unavailable.
EOF
            exit 0 ;;
        *)
            echo "error: unknown argument '$1' (use --help)" >&2
            exit 1 ;;
    esac
done

if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
    echo "error: --jobs must be a positive integer" >&2
    exit 1
fi
if ! command -v rg >/dev/null 2>&1; then
    echo "error: ripgrep (rg) is required" >&2
    exit 1
fi

# LLVM packages commonly install only versioned commands. Pass the selected
# clang-tidy binary explicitly; the runner otherwise invokes an unversioned
# clang-tidy even when run-clang-tidy-21 was discovered successfully.
find_llvm_tool() {
    local name="$1"
    local explicit="$2"
    local candidate
    if [[ -n "$explicit" ]]; then
        command -v "$explicit"
        return
    fi
    if command -v "$name"; then
        return
    fi
    while IFS= read -r candidate; do
        if [[ "$candidate" =~ ^${name}-[0-9]+$ ]]; then
            command -v "$candidate"
            return
        fi
    done < <(compgen -c | sort -uVr)
    return 1
}

if ! tidy_binary="$(find_llvm_tool clang-tidy "${HEAVEN_CLANG_TIDY:-}")"; then
    echo "error: clang-tidy is required (install clang-tidy-21 or set HEAVEN_CLANG_TIDY)" >&2
    exit 1
fi
runner_override="${HEAVEN_RUN_CLANG_TIDY:-}"
if [[ -z "$runner_override" ]]; then
    matching_runner="${tidy_binary%/*}/run-${tidy_binary##*/}"
    if [[ -x "$matching_runner" ]]; then
        runner_override="$matching_runner"
    fi
fi
if ! tidy_runner="$(find_llvm_tool run-clang-tidy "$runner_override")"; then
    echo "error: run-clang-tidy is required (install clang-tidy-21 or set HEAVEN_RUN_CLANG_TIDY)" >&2
    exit 1
fi

# The wrapper reads clang-tidy's structured diagnostics and accepts only the
# fingerprinted Filament quaternion baseline. All original output is retained.
if ! python3 -c 'import yaml' >/dev/null 2>&1; then
    echo "error: Python 3 with PyYAML is required (install python3-yaml)" >&2
    exit 1
fi
python3 -B "${REPO_ROOT}/tests/clang_tidy_gate_test.py"
python3 -B "${REPO_ROOT}/tests/clang_tidy_coverage_test.py"
export HEAVEN_CLANG_TIDY_REAL="$tidy_binary"
tidy_binary="${REPO_ROOT}/scripts/clang_tidy_gate.py"

for compile_db in \
    "${tools_build}/compile_commands.json" \
    "${renderer_build}/compile_commands.json"; do
    if [[ ! -f "${compile_db}" ]]; then
        echo "error: missing ${compile_db}; configure the corresponding build first" >&2
        exit 1
    fi
done

# Enumerate the owned directories directly so source release archives work
# without a .git directory and newly added source files are audited as well.
tool_sources=("${REPO_ROOT}/tools/extraction/uniginex.cpp" "${REPO_ROOT}/tools/extraction/mesh2obj.cpp")
owned_directories=(
    "${REPO_ROOT}/benchmark/src"
    "${REPO_ROOT}/benchmark/tests"
    "${REPO_ROOT}/benchmark/smoke"
    "${REPO_ROOT}/tools/asset_packer"
)
mapfile -t renderer_sources < <(
    rg --files --no-ignore "${owned_directories[@]}" -g '*.cpp' \
        -g '!materials_data.cpp' | sort
)

mapfile -t owned_cpp_files < <(
    rg --files --no-ignore "${owned_directories[@]}" -g '*.cpp' -g '*.h' \
        -g '!materials_data.cpp' | sort
)
owned_cpp_files+=("${tool_sources[@]}")

# run-clang-tidy silently ignores files absent from its compilation database.
# Require ordinary owned sources and disclose the exact optional SDK paths;
# enabled providers and fixtures must have their real compile context.
python3 -B "${REPO_ROOT}/scripts/clang_tidy_coverage.py" --root "${REPO_ROOT}" \
    --compile-db "${tools_build}/compile_commands.json" "${tool_sources[@]}"
python3 -B "${REPO_ROOT}/scripts/clang_tidy_coverage.py" --root "${REPO_ROOT}" \
    --compile-db "${renderer_build}/compile_commands.json" \
    --cmake-cache "${renderer_build}/CMakeCache.txt" "${renderer_sources[@]}"

# Clang deliberately exempts cast-to-void expressions from -Wold-style-cast,
# and clang-tidy's cast checks follow that behavior. Reject the loophole before
# analysis so unused parameters stay visible to misc-unused-parameters. Prefer
# an unnamed parameter (or [[maybe_unused]] when a name improves the contract).
if void_casts="$(
    rg -n --no-heading \
        '\([[:space:]]*void[[:space:]]*\)[[:space:]]*[[:alnum:]_(*&]' \
        "${owned_cpp_files[@]}"
)"; then
    echo "error: C-style cast-to-void suppressions are forbidden:" >&2
    printf '%s\n' "${void_casts}" >&2
    exit 1
fi

"${tidy_runner}" -quiet -j "$jobs" -clang-tidy-binary "$tidy_binary" \
    -p "${tools_build}" "${tool_sources[@]}"
"${tidy_runner}" -quiet -j "$jobs" -clang-tidy-binary "$tidy_binary" \
    -p "${renderer_build}" "${renderer_sources[@]}"
