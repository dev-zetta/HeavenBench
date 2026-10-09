#!/usr/bin/env bash
# HeavenBench — run script
#
# Launches the benchmark executable with sensible defaults for the
# Filament Vulkan backend. On Linux, the launcher enumerates DRM GPUs and
# selects the device with the most dedicated video memory by default. Filament
# is the only renderer now (the legacy hand-rolled GL 4.5 backend was
# removed in v0.4); the `--api` flag is still accepted for
# backward compatibility but is silently a no-op.
#
# Usage:
#   ./scripts/run.sh                       # default settings
#   ./scripts/run.sh --benchmark           # camera-path playback for FPS measurement
#   ./scripts/run.sh --width 2560 --height 1440
#   ./scripts/run.sh --build               # rebuild before running
#   ./scripts/run.sh --compiler aocc       # run the local AOCC build
#   ./scripts/run.sh --list-gpus            # show detected GPUs
#   ./scripts/run.sh --gpu 0                # select a detected GPU by index
#   ./scripts/run.sh --gpu integrated       # prefer the lowest-VRAM GPU
#
# Any unrecognised arguments are forwarded to the benchmark binary.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." &>/dev/null && pwd)"
# shellcheck source=build_paths.sh
source "$SCRIPT_DIR/build_paths.sh"

BUILD_DIR="${HEAVEN_BUILD_DIR:-}"
COMPILER="${HEAVEN_COMPILER:-clang}"
CPU_ARCH="${HEAVEN_CPU_ARCH:-portable}"
DATA_DIR="$REPO_ROOT/assets"
CONFIG_FILE="$REPO_ROOT/config.json"

GPU_SELECT="auto"
GPU_SELECT_EXPLICIT=0
LIST_GPUS=0
DO_BUILD=0
WSI=""                 # "" = SDL default, "wayland" or "x11" to force
PASS_ARGS=()

GPU_BDFS=()
GPU_IDS=()
GPU_VRAM_BYTES=()
GPU_NAMES=()

discover_gpus() {
    local device_path class_code vendor device bdf vram name
    local device_paths=(/sys/class/drm/card*/device)

    for device_path in "${device_paths[@]}"; do
        [[ -r "$device_path/vendor" && -r "$device_path/device" ]] || continue

        class_code=""
        [[ -r "$device_path/class" ]] && read -r class_code < "$device_path/class"
        [[ -z "$class_code" || "$class_code" == 0x03* ]] || continue

        read -r vendor < "$device_path/vendor"
        read -r device < "$device_path/device"
        vendor="${vendor#0x}"
        device="${device#0x}"
        bdf="$(basename -- "$(readlink -f -- "$device_path")")"

        vram=0
        if [[ -r "$device_path/mem_info_vram_total" ]]; then
            read -r vram < "$device_path/mem_info_vram_total"
        fi
        [[ "$vram" =~ ^[0-9]+$ ]] || vram=0

        name=""
        if command -v lspci >/dev/null 2>&1; then
            name="$(lspci -s "$bdf" 2>/dev/null | sed 's/^[^ ]* //')"
        fi
        [[ -n "$name" ]] || name="PCI GPU ${vendor}:${device}"

        GPU_BDFS+=("$bdf")
        GPU_IDS+=("${vendor,,}:${device,,}")
        GPU_VRAM_BYTES+=("$vram")
        GPU_NAMES+=("$name")
    done
}

format_vram() {
    local bytes="$1"
    if (( bytes > 0 )); then
        printf '%d MiB' "$((bytes / 1024 / 1024))"
    else
        printf 'VRAM unknown'
    fi
}

print_gpus() {
    local i
    if (( ${#GPU_IDS[@]} == 0 )); then
        echo "No DRM GPUs detected."
        return
    fi

    echo "Detected GPUs:"
    for ((i = 0; i < ${#GPU_IDS[@]}; ++i)); do
        printf '  [%d] %s [%s, %s, %s]\n' \
            "$i" "${GPU_NAMES[$i]}" "${GPU_IDS[$i]}" "${GPU_BDFS[$i]}" \
            "$(format_vram "${GPU_VRAM_BYTES[$i]}")"
    done
}

gpu_index_for_policy() {
    local policy="$1" best=0 i
    for ((i = 1; i < ${#GPU_IDS[@]}; ++i)); do
        if [[ "$policy" == "lowest" ]]; then
            (( GPU_VRAM_BYTES[i] < GPU_VRAM_BYTES[best] )) && best="$i"
        else
            (( GPU_VRAM_BYTES[i] > GPU_VRAM_BYTES[best] )) && best="$i"
        fi
    done
    printf '%d' "$best"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build|-b)
            DO_BUILD=1
            shift ;;
        --build-dir)
            if [[ $# -lt 2 ]]; then
                echo "Error: --build-dir requires a directory" >&2
                exit 2
            fi
            BUILD_DIR="$2"
            shift 2 ;;
        --compiler)
            if [[ $# -lt 2 ]]; then
                echo "Error: --compiler requires clang or aocc" >&2
                exit 2
            fi
            COMPILER="$2"
            shift 2 ;;
        --cpu-arch)
            if [[ $# -lt 2 ]]; then
                echo "Error: --cpu-arch requires an architecture" >&2
                exit 2
            fi
            CPU_ARCH="$2"
            shift 2 ;;
        --gpu)
            if [[ $# -lt 2 ]]; then
                echo "Error: --gpu requires a value" >&2
                exit 2
            fi
            GPU_SELECT="$2"
            GPU_SELECT_EXPLICIT=1
            shift 2 ;;
        --list-gpus)
            LIST_GPUS=1
            shift ;;
        --wayland)
            WSI="wayland"
            shift ;;
        --x11)
            WSI="x11"
            shift ;;
        --help|-h)
            cat <<EOF
HeavenBench run script

Usage: $0 [options] [-- <benchmark args>]

Wrapper options:
  --build, -b         Rebuild the benchmark before running
  --build-dir DIR     Use a non-default renderer build tree for both --build
                      and execution
  --compiler NAME     Select the clang or aocc build tree (default: clang)
  --cpu-arch ARCH     CPU target used by --build (portable, native, zen1..zen5)
  --list-gpus         List detected DRM GPUs and exit
  --gpu <selector>    Select GPU by list index, PCI vendor:device ID, or PCI
                      address. "auto"/"dedicated" prefers the most VRAM,
                      "integrated" the least, and "default" leaves selection
                      to the Vulkan driver (default: auto).
  --wayland           Force the native Wayland window backend
  --x11               Force the X11 / XWayland window backend
  --help, -h          Show this help

The benchmark binary supports BOTH X11 and Wayland in a single
build (Filament's Vulkan platform dispatches at runtime). Without
--wayland/--x11, native Wayland is preferred on a Wayland desktop
so compositor scaling is identical for SDR and HDR; elsewhere SDL's
default is used.

Forwarded benchmark options (see --help on the binary for the full list):
  --no-tessellation              Disable CPU tessellation
  --no-heat-haze                 Disable chimney refraction grab pass
  --wind-debug                   Render world-space wind arrows on flags
  --cloth-rate HZ                Cloth simulation rate (default 60)
  --tess-scale F                 Displacement strength multiplier (default 1.0)
  --tess-factor F                Subdivision factor (default 1.0)
  --tess-distance D              Tessellation fade distance (default 50)
  --time-of-day H                Hour of day, 0..24 (default 18.0 = sunset)
  --benchmark                    Run camera-path playback
  --data PATH                    Override loose assets, ZIP, or ZIP folder
  --width N --height N           Window size
  --fullscreen                   Fullscreen window
  --node FILE                    Load a specific .node scene

Environment overrides:
  HEAVEN_BUILD_ROOT  Build root (default: <repository>/build)
  HEAVEN_BUILD_DIR   Renderer build directory (overrides HEAVEN_BUILD_ROOT)
  HEAVEN_COMPILER    Compiler toolchain: clang or aocc
  HEAVEN_CPU_ARCH    CPU target used by --build
  HEAVEN_BIN     Path to the benchmark binary
  HEAVEN_DATA    Loose asset tree, ZIP archive, or ZIP folder
  HEAVEN_CONFIG  Configuration file (default: repository config.json)
  SDL_VIDEODRIVER  Force the SDL window backend (wayland/x11)
  MESA_VK_DEVICE_SELECT / DRI_PRIME  Preserved when --gpu is not specified
EOF
            exit 0 ;;
        *)
            PASS_ARGS+=("$1")
            shift ;;
    esac
done

case "$COMPILER" in
    clang|aocc) ;;
    *)
        echo "Error: unsupported compiler '$COMPILER' (expected clang or aocc)" >&2
        exit 2 ;;
esac

if [[ -z "$BUILD_DIR" ]]; then
    BUILD_DIR="$(
        heaven_renderer_build_dir linux Release arm64-v8a "$COMPILER"
    )"
fi

discover_gpus
if [[ "$LIST_GPUS" -eq 1 ]]; then
    print_gpus
    exit 0
fi

# Allow env overrides. Resolve the binary only after argument parsing so
# --build-dir, HEAVEN_BUILD_DIR, --build, and execution always agree.
BIN="${HEAVEN_BIN:-$BUILD_DIR/heaven_benchmark}"
DATA_DIR="${HEAVEN_DATA:-$DATA_DIR}"
CONFIG_FILE="${HEAVEN_CONFIG:-$CONFIG_FILE}"

if [[ "$DO_BUILD" -eq 1 ]]; then
    echo "==> Building first"
    "$SCRIPT_DIR/build.sh" \
        --compiler "$COMPILER" \
        --cpu-arch "$CPU_ARCH" \
        --build-dir "$BUILD_DIR"
fi

if [[ ! -x "$BIN" ]]; then
    echo "Error: benchmark binary not found at $BIN" >&2
    echo "Run ./scripts/build.sh first, or pass --build" >&2
    exit 1
fi

# Always pass the canonical repository configuration and data directory unless
# the caller explicitly supplied replacements.
HAS_DATA=0
HAS_CONFIG=0
for arg in "${PASS_ARGS[@]:-}"; do
    [[ "$arg" == "--data" ]] && HAS_DATA=1 && break
done
for arg in "${PASS_ARGS[@]:-}"; do
    [[ "$arg" == "--config" ]] && HAS_CONFIG=1 && break
done
if [[ "$HAS_DATA" -eq 0 ]]; then
    if [[ ! -d "$DATA_DIR" && ! -f "$DATA_DIR" ]]; then
        echo "Error: asset tree, ZIP archive, or ZIP folder not found at $DATA_DIR" >&2
        echo "Pack it with --pack-assets, set HEAVEN_DATA, or pass --data <path>" >&2
        exit 1
    fi
    PASS_ARGS+=("--data" "$DATA_DIR")
fi
if [[ "$HAS_CONFIG" -eq 0 ]]; then
    if [[ ! -f "$CONFIG_FILE" ]]; then
        echo "Error: configuration file not found at $CONFIG_FILE" >&2
        echo "Set HEAVEN_CONFIG or pass --config <file>" >&2
        exit 1
    fi
    PASS_ARGS+=("--config" "$CONFIG_FILE")
fi

# Select before the process loads Vulkan. MESA_VK_DEVICE_SELECT addresses the
# Vulkan device while DRI_PRIME's PCI form covers Mesa's general device choice.
# Existing environment overrides win over the automatic policy; an explicit
# --gpu always wins over the environment.
if [[ "$GPU_SELECT_EXPLICIT" -eq 0 && "$GPU_SELECT" == "auto" && \
      ( -n "${MESA_VK_DEVICE_SELECT:-}" || -n "${DRI_PRIME:-}" ) ]]; then
    echo "==> Keeping GPU selection from environment"
elif [[ "$GPU_SELECT" == "default" || -z "$GPU_SELECT" ]]; then
    unset MESA_VK_DEVICE_SELECT DRI_PRIME
    echo "==> GPU selection left to the Vulkan driver"
elif (( ${#GPU_IDS[@]} == 0 )); then
    if [[ "$GPU_SELECT" == "auto" || "$GPU_SELECT" == "dedicated" || \
          "$GPU_SELECT" == "discrete" || "$GPU_SELECT" == "integrated" ]]; then
        echo "Warning: no DRM GPUs detected; GPU selection left to the Vulkan driver" >&2
    else
        export MESA_VK_DEVICE_SELECT="$GPU_SELECT"
        echo "==> MESA_VK_DEVICE_SELECT=$GPU_SELECT (GPU enumeration unavailable)"
    fi
else
    GPU_INDEX=""
    case "$GPU_SELECT" in
        auto|dedicated|discrete)
            GPU_INDEX="$(gpu_index_for_policy highest)" ;;
        integrated)
            GPU_INDEX="$(gpu_index_for_policy lowest)" ;;
        *)
            if [[ "$GPU_SELECT" =~ ^[0-9]+$ ]] &&
               (( GPU_SELECT < ${#GPU_IDS[@]} )); then
                GPU_INDEX="$GPU_SELECT"
            else
                for ((i = 0; i < ${#GPU_IDS[@]}; ++i)); do
                    if [[ "${GPU_IDS[$i]}" == "${GPU_SELECT,,}" ||
                          "${GPU_BDFS[$i]}" == "$GPU_SELECT" ]]; then
                        GPU_INDEX="$i"
                        break
                    fi
                done
            fi ;;
    esac

    if [[ -z "$GPU_INDEX" ]]; then
        echo "Error: GPU selector '$GPU_SELECT' did not match a detected GPU" >&2
        print_gpus >&2
        exit 2
    fi

    GPU_PCI_PRIME="${GPU_BDFS[$GPU_INDEX]//:/_}"
    GPU_PCI_PRIME="${GPU_PCI_PRIME//./_}"
    export MESA_VK_DEVICE_SELECT="${GPU_IDS[$GPU_INDEX]}"
    export DRI_PRIME="pci-${GPU_PCI_PRIME}"
    echo "==> GPU [$GPU_INDEX]: ${GPU_NAMES[$GPU_INDEX]} ($(format_vram "${GPU_VRAM_BYTES[$GPU_INDEX]}"))"
    echo "==> MESA_VK_DEVICE_SELECT=$MESA_VK_DEVICE_SELECT DRI_PRIME=$DRI_PRIME"
fi

# Optionally force the SDL window backend. The binary supports both
# X11 and Wayland; this just pins which one SDL hands to Filament.
if [[ -n "$WSI" ]]; then
    export SDL_VIDEODRIVER="$WSI"
    echo "==> SDL_VIDEODRIVER=$WSI"
fi

echo "==> Running: $BIN ${PASS_ARGS[*]}"
exec "$BIN" "${PASS_ARGS[@]}"
