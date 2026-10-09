#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
TOOLS_DIR="${REPO_DIR}/build/hires-texture-tools"
DATA_PATH=""
OUTPUT_PATH=""
DEVICE="cpu"
EXTRA_ARGS=()

usage() {
    cat <<EOF
Usage: $0 --data PATH --output PATH --device cpu|cuda|rocm [options]

Options forwarded to the generator:
  --limit N         Bake the first N entries (developer smoke only)
  --resume          Reuse validated content-addressed outputs (default)
  --revalidate      Verify/reclassify existing outputs without using the GPU
  --force           Rebuild every texture
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --data) DATA_PATH="${2:?missing --data value}"; shift 2 ;;
        --output) OUTPUT_PATH="${2:?missing --output value}"; shift 2 ;;
        --device) DEVICE="${2:?missing --device value}"; shift 2 ;;
        --limit) EXTRA_ARGS+=("$1" "${2:?missing value for $1}"); shift 2 ;;
        --resume|--revalidate|--force) EXTRA_ARGS+=("$1"); shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

[[ -n "${DATA_PATH}" && -n "${OUTPUT_PATH}" ]] || {
    usage >&2
    exit 2
}
case "${DEVICE}" in cpu|cuda|rocm) ;; *) echo "invalid --device" >&2; exit 2 ;; esac
VENV_DIR="${TOOLS_DIR}/venv-${DEVICE}"
TOOLCHAIN_ENV="${TOOLS_DIR}/toolchain-${DEVICE}.env"

if [[ ! -x "${VENV_DIR}/bin/python" || \
      ! -f "${TOOLCHAIN_ENV}" ]]; then
    echo "Hi-res tools are not installed. Run:" >&2
    echo "  ${SCRIPT_DIR}/setup_hires_texture_tools.sh --torch ${DEVICE}" >&2
    exit 1
fi

set -a
source "${TOOLCHAIN_ENV}"
set +a
export PYTHONPATH="${HEAVEN_HIRES_BASICSR}${PYTHONPATH:+:${PYTHONPATH}}"

exec "${VENV_DIR}/bin/python" \
    "${REPO_DIR}/tools/hires_textures/hires_texture_generator.py" \
    --data "${DATA_PATH}" --output "${OUTPUT_PATH}" --device "${DEVICE}" \
    "${EXTRA_ARGS[@]}"
