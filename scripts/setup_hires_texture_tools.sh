#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
TOOLS_DIR="${REPO_DIR}/build/hires-texture-tools"
TORCH_FLAVOR="cpu"
PYTHON_BIN=""
UV_BIN=""
export PIP_DISABLE_PIP_VERSION_CHECK=1

usage() {
    echo "Usage: $0 [--torch cpu|cuda|rocm] [--python PATH]"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --torch) TORCH_FLAVOR="${2:?missing --torch value}"; shift 2 ;;
        --python) PYTHON_BIN="${2:?missing --python value}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

case "${TORCH_FLAVOR}" in
    cpu|cuda|rocm) ;;
    *) echo "--torch must be cpu, cuda, or rocm" >&2; exit 2 ;;
esac

case "${TORCH_FLAVOR}" in
    rocm)
        REQUIRED_PYTHON="3.12"
        UV_PYTHON="3.12.10"
        ;;
    cpu|cuda)
        REQUIRED_PYTHON="3.11"
        UV_PYTHON="3.11.9"
        ;;
esac
VENV_DIR="${TOOLS_DIR}/venv-${TORCH_FLAVOR}"
TOOLCHAIN_ENV="${TOOLS_DIR}/toolchain-${TORCH_FLAVOR}.env"

if [[ -z "${PYTHON_BIN}" ]]; then
    if command -v "python${REQUIRED_PYTHON}" >/dev/null 2>&1; then
        PYTHON_BIN="$(command -v "python${REQUIRED_PYTHON}")"
    elif command -v uv >/dev/null 2>&1; then
        UV_BIN="$(command -v uv)"
    else
        echo "Python ${REQUIRED_PYTHON} or uv is required for ${TORCH_FLAVOR}" >&2
        exit 1
    fi
fi
if [[ -n "${PYTHON_BIN}" ]] && ! "${PYTHON_BIN}" -c \
    'import sys; expected=tuple(map(int, sys.argv[1].split("."))); raise SystemExit(sys.version_info[:2] != expected)' \
    "${REQUIRED_PYTHON}"; then
    echo "--python must select Python ${REQUIRED_PYTHON} for ${TORCH_FLAVOR}" >&2
    exit 1
fi

mkdir -p "${TOOLS_DIR}/downloads" "${TOOLS_DIR}/src"

fetch_verified() {
    local url="$1"
    local output="$2"
    local expected="$3"
    if [[ ! -f "${output}" ]] || \
       [[ "$(sha256sum "${output}" | awk '{print $1}')" != "${expected}" ]]; then
        curl -L --fail --retry 3 --output "${output}.part" "${url}"
        mv "${output}.part" "${output}"
    fi
    echo "${expected}  ${output}" | sha256sum --check --status
}

HAT_REV="1638a9a822581657811867bf670717f8371fc3e5"
BASICSR_REV="5668ba75eb8a77e8d2dd46746a36fee0fbb0fdcd"
COMPRESSONATOR_REV="80284f3544a9ebb2a47b49691213be77929a0727"
HAT_ARCHIVE="${TOOLS_DIR}/downloads/hat-${HAT_REV}.tar.gz"
BASICSR_ARCHIVE="${TOOLS_DIR}/downloads/basicsr-${BASICSR_REV}.tar.gz"
COMPRESSONATOR_ARCHIVE="${TOOLS_DIR}/downloads/compressonator-${COMPRESSONATOR_REV}.tar.gz"
COMPRESSONATOR_CLI_ARCHIVE="${TOOLS_DIR}/downloads/compressonatorcli-4.4.19-Linux.tar.gz"

fetch_verified \
  "https://codeload.github.com/XPixelGroup/HAT/tar.gz/${HAT_REV}" \
  "${HAT_ARCHIVE}" \
  "e18c17922ca3c12d181cc364ee0f5e25daf7fcf166ae2761ef77beeecde8dad3"
fetch_verified \
  "https://codeload.github.com/XPixelGroup/BasicSR/tar.gz/${BASICSR_REV}" \
  "${BASICSR_ARCHIVE}" \
  "79822f3537d6ea7ff90484ed303893ff1b80d24251ef3f8d227a4f634fd3e9bf"
fetch_verified \
  "https://codeload.github.com/GPUOpen-Tools/compressonator/tar.gz/${COMPRESSONATOR_REV}" \
  "${COMPRESSONATOR_ARCHIVE}" \
  "b1ec393a86e001c4e81549ca375ce59ed3f6b737efd52d10b344c48391aaeb98"
fetch_verified \
  "https://github.com/GPUOpen-Tools/compressonator/releases/download/V4.4.19/compressonatorcli-4.4.19-Linux.tar.gz" \
  "${COMPRESSONATOR_CLI_ARCHIVE}" \
  "92b9e4bc5388b4c839200535a88402009fb3e8c3b97645999ec54e510f798671"

extract_once() {
    local archive="$1"
    local destination="$2"
    local marker="$3"
    if [[ ! -f "${destination}/${marker}" ]]; then
        mkdir -p "${destination}"
        tar -xzf "${archive}" --strip-components=1 -C "${destination}"
        touch "${destination}/${marker}"
    fi
}

extract_once "${HAT_ARCHIVE}" "${TOOLS_DIR}/src/HAT" ".heavenbench-pinned"
extract_once "${BASICSR_ARCHIVE}" "${TOOLS_DIR}/src/BasicSR" ".heavenbench-pinned"
extract_once "${COMPRESSONATOR_ARCHIVE}" \
             "${TOOLS_DIR}/src/compressonator" ".heavenbench-pinned"

BASICSR_PATCH="${REPO_DIR}/tools/hires_textures/patches/basicsr-torchvision-functional.patch"
BASICSR_PATCH_SHA256="da33b2cdfa111798addf0bc348dc15b951aef2931be09b52117f4250d0adfd35"
BASICSR_PATCH_MARKER="${TOOLS_DIR}/src/BasicSR/.heavenbench-torchvision-functional-${BASICSR_PATCH_SHA256}"
echo "${BASICSR_PATCH_SHA256}  ${BASICSR_PATCH}" | sha256sum --check --status
if [[ ! -f "${BASICSR_PATCH_MARKER}" ]]; then
    if grep -q "except ModuleNotFoundError" \
        "${TOOLS_DIR}/src/BasicSR/basicsr/data/degradations.py"; then
        touch "${BASICSR_PATCH_MARKER}"
    else
        patch --fuzz=0 -d "${TOOLS_DIR}/src/BasicSR" -p1 < "${BASICSR_PATCH}"
        touch "${BASICSR_PATCH_MARKER}"
    fi
fi

if [[ ! -x "${VENV_DIR}/bin/python" ]]; then
    if [[ -n "${UV_BIN}" ]]; then
        "${UV_BIN}" venv --seed --python "${UV_PYTHON}" "${VENV_DIR}"
    else
        "${PYTHON_BIN}" -m venv "${VENV_DIR}"
    fi
fi
VENV_PY="${VENV_DIR}/bin/python"
if ! "${VENV_PY}" -m pip --version >/dev/null 2>&1; then
    if command -v uv >/dev/null 2>&1; then
        "$(command -v uv)" pip install --python "${VENV_PY}" \
            "pip==24.2" "setuptools==72.1.0" "wheel==0.44.0"
    else
        "${VENV_PY}" -m ensurepip --upgrade
    fi
fi
"${VENV_PY}" -m pip install --upgrade "pip==24.2" "setuptools==72.1.0" \
    "wheel==0.44.0"
if "${VENV_PY}" -m pip show basicsr >/dev/null 2>&1; then
    "${VENV_PY}" -m pip uninstall --yes basicsr
fi
"${VENV_PY}" -m pip install "numpy==1.26.4"

case "${TORCH_FLAVOR}" in
    cpu|cuda)
        if [[ "${TORCH_FLAVOR}" == "cpu" ]]; then
            TORCH_INDEX="https://download.pytorch.org/whl/cpu"
        else
            TORCH_INDEX="https://download.pytorch.org/whl/cu121"
        fi
        "${VENV_PY}" -m pip install --index-url "${TORCH_INDEX}" \
            "torch==2.1.2" "torchvision==0.16.2"
        ;;
    rocm)
        ROCM_WHEEL_BASE="https://repo.radeon.com/rocm/manylinux/rocm-rel-7.2.1"
        if ! "${VENV_PY}" -c \
            'import torch, torchvision, triton; assert torch.__version__ == "2.9.1+rocm7.2.1.gitff65f5bc"; assert torchvision.__version__ == "0.24.0+rocm7.2.1.gitb919bd0c"; assert triton.__version__ == "3.5.1+rocm7.2.1.gita272dfa8"' \
            >/dev/null 2>&1; then
            "${VENV_PY}" -m pip install \
                "${ROCM_WHEEL_BASE}/torch-2.9.1%2Brocm7.2.1.lw.gitff65f5bc-cp312-cp312-linux_x86_64.whl" \
                "${ROCM_WHEEL_BASE}/torchvision-0.24.0%2Brocm7.2.1.gitb919bd0c-cp312-cp312-linux_x86_64.whl" \
                "${ROCM_WHEEL_BASE}/triton-3.5.1%2Brocm7.2.1.gita272dfa8-cp312-cp312-linux_x86_64.whl"
        fi
        ;;
esac
"${VENV_PY}" -m pip install \
    "numpy==1.26.4" "opencv-python-headless==4.10.0.84" \
    "Pillow==10.4.0" "scipy==1.13.1" "scikit-image==0.24.0" \
    "tifffile==2024.8.30" \
    "PyYAML==6.0.2" "tqdm==4.66.5" "requests==2.32.3" \
    "timm==0.9.16" "einops==0.8.0"
PYTHONPATH="${TOOLS_DIR}/src/BasicSR:${TOOLS_DIR}/src/HAT" \
    "${VENV_PY}" -c \
    'from basicsr.data.degradations import rgb_to_grayscale; from hat.archs.hat_arch import HAT; print("HAT/BasicSR imports ready")'

case "${TORCH_FLAVOR}" in
    rocm)
        if [[ ! -e /dev/kfd ]]; then
            echo "ROCm device /dev/kfd is unavailable" >&2
            exit 1
        fi
        "${VENV_PY}" -c \
            'import torch; assert torch.version.hip, "not a ROCm PyTorch build"; assert torch.cuda.is_available(), "ROCm GPU unavailable"; print("ROCm PyTorch", torch.__version__, "HIP", torch.version.hip); [print(f"  GPU {i}: {torch.cuda.get_device_name(i)}") for i in range(torch.cuda.device_count())]'
        ;;
    cuda)
        "${VENV_PY}" -c \
            'import torch; assert torch.version.cuda and not torch.version.hip, "not a CUDA PyTorch build"; assert torch.cuda.is_available(), "CUDA GPU unavailable"; print("CUDA PyTorch", torch.__version__, torch.cuda.get_device_name(0))'
        ;;
    cpu)
        "${VENV_PY}" -c \
            'import torch; print("CPU PyTorch", torch.__version__)'
        ;;
esac
TORCH_BUILD="$("${VENV_PY}" -c 'import torch; print(torch.__version__)')"

MODEL="${TOOLS_DIR}/models/Real_HAT_GAN_SRx4.pth"
mkdir -p "${TOOLS_DIR}/models"
fetch_verified \
  "https://huggingface.co/AEmotionStudio/ai-upscale-models/resolve/main/Real_HAT_GAN_SRx4.pth?download=true" \
  "${MODEL}" \
  "f5b1e3bbbb05147ca2beefcc715279cb647d7976cbda67d62ea7e6e20d5ffcc7"

CMP_OUT="${TOOLS_DIR}/compressonator"
CMP_MARKER="${CMP_OUT}/.heavenbench-cli-4.4.19-92b9e4bc"
if [[ ! -f "${CMP_MARKER}" ]]; then
    mkdir -p "${CMP_OUT}"
    tar -xzf "${COMPRESSONATOR_CLI_ARCHIVE}" --strip-components=1 -C "${CMP_OUT}"
    touch "${CMP_MARKER}"
fi
CLI_PATH="${CMP_OUT}/compressonatorcli"
if [[ ! -x "${CLI_PATH}" ]] || \
   [[ "$("${CLI_PATH}" -version 2>&1)" != *"version 4.4.19"* ]]; then
    echo "Pinned CompressonatorCLI 4.4.19 is unavailable" >&2
    exit 1
fi
cat > "${TOOLS_DIR}/CompressonatorCLI" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

wrapper_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cmp_dir="${wrapper_dir}/compressonator"
export LD_LIBRARY_PATH="${cmp_dir}:${cmp_dir}/qt:${cmp_dir}/pkglibs${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
exec "${cmp_dir}/compressonatorcli-bin" "$@"
EOF
chmod +x "${TOOLS_DIR}/CompressonatorCLI"
if [[ "$("${TOOLS_DIR}/CompressonatorCLI" -version 2>&1)" != \
      *"version 4.4.19"* ]]; then
    echo "CompressonatorCLI wrapper validation failed" >&2
    exit 1
fi

PYTHONPATH="${REPO_DIR}/tools/hires_textures" "${VENV_PY}" \
    "${REPO_DIR}/tools/hires_textures/test_hires_texture_generator.py"

cat > "${TOOLCHAIN_ENV}" <<EOF
HEAVEN_HIRES_HAT=${TOOLS_DIR}/src/HAT
HEAVEN_HIRES_BASICSR=${TOOLS_DIR}/src/BasicSR
HEAVEN_HIRES_BASICSR_PATCH=${BASICSR_PATCH}
HEAVEN_HIRES_COMPRESSONATOR_SRC=${TOOLS_DIR}/src/compressonator
HEAVEN_HIRES_MODEL=${MODEL}
HEAVEN_HIRES_COMPRESSONATOR=${TOOLS_DIR}/CompressonatorCLI
HEAVEN_HIRES_COMPRESSONATOR_CLI_SHA256=92b9e4bc5388b4c839200535a88402009fb3e8c3b97645999ec54e510f798671
HEAVEN_HIRES_TORCH=${TORCH_FLAVOR}
HEAVEN_HIRES_TORCH_BUILD=${TORCH_BUILD}
EOF

echo "Hi-res ${TORCH_FLAVOR} texture tools ready under ${VENV_DIR}"
