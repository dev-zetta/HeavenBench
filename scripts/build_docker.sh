#!/usr/bin/env bash
# The same isolated build/test/package entry point is used locally and in CI.
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"
usage() {
    cat <<'EOF'
Usage: scripts/build_docker.sh linux|android [options]
  --cpu-arch portable|zen1|zen2|zen3|zen4|zen5   Linux target (default portable)
  --abi arm64-v8a|x86_64                       Android target (default arm64-v8a)
  --jobs N                                   Parallel compile jobs (default 2)
  --output-dir DIR                           Default build/release/<platform>-<target>
  --clean                                    Fresh layers and a new private build cache
  --skip-analysis                            Skip Linux clang-tidy (developer builds)
  --experimental-fsr3                        SDK-enabled portable Linux artifact, separate cache

Requires Docker with Buildx and Bash. DOCKER_CONTEXT selects the Docker daemon.
Build hosts use linux/amd64; ARM hosts need Docker's x86-64 emulation.
No host compiler, SDK, vendor checkout, original assets, or GPU is used.
EOF
}
[[ $# -gt 0 ]] || { usage >&2; exit 2; }
case "$1" in --help|-h) usage; exit 0 ;; linux|android) PLATFORM="$1"; shift ;; *) usage >&2; exit 2 ;; esac
CPU_ARCH=portable
ABI=arm64-v8a
JOBS=2
OUTPUT_DIR=
CLEAN=0
RUN_ANALYSIS=1
EXPERIMENTAL_FSR3=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --cpu-arch|--abi|--jobs|--output-dir)
            [[ $# -ge 2 && -n "$2" ]] || { echo "Missing value for $1" >&2; exit 2; } ;;
    esac
    case "$1" in
        --cpu-arch) [[ "$PLATFORM" == linux ]] || { echo '--cpu-arch requires linux' >&2; exit 2; }; CPU_ARCH="$2"; shift 2 ;;
        --abi) [[ "$PLATFORM" == android ]] || { echo '--abi requires android' >&2; exit 2; }; ABI="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --clean) CLEAN=1; shift ;;
        --skip-analysis) [[ "$PLATFORM" == linux ]] || { echo '--skip-analysis requires linux' >&2; exit 2; }; RUN_ANALYSIS=0; shift ;;
        --experimental-fsr3) [[ "$PLATFORM" == linux ]] || { echo '--experimental-fsr3 requires linux' >&2; exit 2; }; EXPERIMENTAL_FSR3=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo '--jobs must be positive' >&2; exit 2; }
case "$CPU_ARCH" in portable|zen[1-5]) ;; *) echo "Unsupported CPU architecture: $CPU_ARCH" >&2; exit 2 ;; esac
case "$ABI" in arm64-v8a|x86_64) ;; *) echo "Unsupported Android ABI: $ABI" >&2; exit 2 ;; esac
TARGET="$CPU_ARCH"
[[ "$PLATFORM" != android ]] || TARGET="$ABI"
ARTIFACT_STAGE="$PLATFORM-artifacts"
if [[ "$EXPERIMENTAL_FSR3" == 1 ]]; then
    [[ "$CPU_ARCH" == portable ]] || { echo 'Experimental FSR distribution requires portable CPU' >&2; exit 2; }
    TARGET=experimental-fsr3
    ARTIFACT_STAGE=linux-fsr3-artifacts
fi
OUTPUT_DIR="${OUTPUT_DIR:-$REPO_ROOT/build/release/$PLATFORM-$TARGET}"
# Buildx's local exporter uses comma-separated options. Fail rather than
# misinterpreting an output path as another exporter setting.
[[ "$OUTPUT_DIR" != *','* && "$OUTPUT_DIR" != *$'\n'* && "$OUTPUT_DIR" != *$'\r'* ]] || {
    echo '--output-dir cannot contain commas or newlines' >&2; exit 2;
}
command -v docker >/dev/null || { echo 'Docker with Buildx is required.' >&2; exit 1; }
mkdir -p -- "$OUTPUT_DIR"
OUTPUT_DIR="$(cd -- "$OUTPUT_DIR" && pwd -P)"
case "$OUTPUT_DIR" in
    "$REPO_ROOT/build/"*) ;;
    "$REPO_ROOT"|"$REPO_ROOT/"*|/) echo 'Inside the repository, artifacts must be under build/.' >&2; exit 2 ;;
esac
CACHE_SCOPE=default
extra_args=()
if [[ "$CLEAN" == 1 ]]; then
    # --no-cache alone does not empty RUN cache mounts. A fresh ID does, without
    # deleting anyone else's artifacts, images or build caches.
    CACHE_SCOPE="clean-$(date +%s)-$$-$RANDOM"
    extra_args+=(--no-cache)
fi
mkdir -p "$OUTPUT_DIR/logs"
# Bash 3.2 treats an empty array as unset under nounset; preserve zero arguments.
docker buildx build --platform linux/amd64 --file "$REPO_ROOT/docker/Dockerfile" \
    --target "$ARTIFACT_STAGE" --progress plain \
    --build-arg "JOBS=$JOBS" --build-arg "CPU_ARCH=$CPU_ARCH" \
    --build-arg "ANDROID_ABI=$ABI" --build-arg "CACHE_SCOPE=$CACHE_SCOPE" \
    --build-arg "RUN_ANALYSIS=$RUN_ANALYSIS" \
    --output "type=local,dest=$OUTPUT_DIR" ${extra_args[@]+"${extra_args[@]}"} "$REPO_ROOT" \
    2>&1 | tee "$OUTPUT_DIR/logs/docker-build.log"
