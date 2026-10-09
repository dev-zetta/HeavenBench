#!/usr/bin/env bash
# Compare full camera-path runs with a frozen configuration and isolated results.
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." &>/dev/null && pwd)"
source "$SCRIPT_DIR/build_paths.sh"
PROFILE_DIR="${HEAVEN_PROFILE_DIR:-$HEAVEN_BUILD_ROOT/profiles}"

NAMES=(all_features no_grass no_volumetric_clouds no_volumetric_fog no_ssgi
       no_point_light_shadows no_screen_space_depth_effects no_gpu_instancing)
ONLY=""
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    cat <<'HELP'
Usage: scripts/profile.sh [VARIANT] [benchmark/run.sh options...]

Run one full camera-path comparison or the default feature-ablation suite.
Variants: all_features, no_grass, no_volumetric_clouds, no_volumetric_fog,
          no_ssgi, no_point_light_shadows, no_screen_space_depth_effects,
          no_gpu_instancing, no_tessellation, no_contact_shadows,
          no_ambient_occlusion, no_msaa, sun_shadow_2048, half_resolution.

The base is the saved configuration at 1920x1080; trailing options override it.
Each invocation freezes effective settings and keeps logs, JSON/HTML results,
exit status and commands in a fresh build/profiles/run-* directory.
Feature removal and half_resolution are diagnostic workload changes, not
quality-preserving optimizations. Compare identical APIs, GPUs and assets.
Use --x11 on Linux when the Wayland compositor forces FIFO presentation.
no_screen_space_depth_effects removes clouds, fog, god rays, SSGI and heat haze;
AO, SSR and contact shadows remain enabled. Quality level 0 means Low, not off.

Environment: HEAVEN_PROFILE_DIR, HEAVEN_PROFILE_TIMEOUT (seconds, default 600),
             plus the HEAVEN_BIN/HEAVEN_DATA overrides accepted by run.sh.
HELP
    exit 0
fi
if [[ $# -gt 0 && "$1" != --* ]]; then ONLY=$1; shift; fi
case "$ONLY" in
    "") ;;
    all_features|no_grass|no_volumetric_clouds|no_volumetric_fog|no_ssgi|no_point_light_shadows|no_screen_space_depth_effects|no_gpu_instancing|no_tessellation|no_contact_shadows|no_ambient_occlusion|no_msaa|sun_shadow_2048|half_resolution) NAMES=("$ONLY") ;;
    *) echo "Unknown variant: $ONLY (see --help)" >&2; exit 2 ;;
esac
PROFILE_TIMEOUT=${HEAVEN_PROFILE_TIMEOUT:-600}
[[ "$PROFILE_TIMEOUT" =~ ^[1-9][0-9]*$ ]] || { echo 'HEAVEN_PROFILE_TIMEOUT must be positive seconds' >&2; exit 2; }
mkdir -p "$PROFILE_DIR"
PROFILE_DIR=$(realpath "$PROFILE_DIR")
RUN_DIR=$(mktemp -d "$PROFILE_DIR/run-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")
SNAPSHOT="$RUN_DIR/base-config.json"
COMMON_ARGS=(--benchmark --width 1920 --height 1080 "$@")
# Resolve preset overlays and all user overrides once, before changing features.
HEAVEN_USER_DIR="$RUN_DIR/config-user" "$SCRIPT_DIR/run.sh" \
    "${COMMON_ARGS[@]}" --write-config "$SNAPSHOT" > "$RUN_DIR/config.log" 2>&1
python3 - "$RUN_DIR" "$REPO_ROOT" <<'PY'
from pathlib import Path
import json, os, platform, subprocess, sys
out, repo = map(Path, sys.argv[1:])
revision = subprocess.run(['git','-C',str(repo),'rev-parse','HEAD'], capture_output=True, text=True)
(out/'environment.json').write_text(json.dumps({
    'source_commit': revision.stdout.strip() if revision.returncode == 0 else None,
    'source_dirty': bool(subprocess.run(['git','-C',str(repo),'status','--porcelain'], capture_output=True, text=True).stdout.strip()) if revision.returncode == 0 else None,
    'platform': platform.platform(),
    'overrides': {k: os.environ[k] for k in ('HEAVEN_BIN','HEAVEN_DATA','SDL_VIDEODRIVER','MESA_VK_DEVICE_SELECT','DRI_PRIME') if k in os.environ},
}, indent=2)+'\n')
PY
for name in "${NAMES[@]}"; do
    variant_args=()
    case "$name" in
        no_grass) variant_args=(--no-grass --no-massive-foliage) ;;
        no_volumetric_clouds) variant_args=(--no-volumetric-clouds) ;;
        no_volumetric_fog) variant_args=(--no-volumetric-fog) ;;
        no_ssgi) variant_args=(--no-ssgi) ;;
        no_point_light_shadows) variant_args=(--no-point-light-shadows) ;;
        no_screen_space_depth_effects) variant_args=(--no-god-rays --no-volumetric-fog --no-volumetric-clouds --no-ssgi --no-heat-haze) ;;
        no_gpu_instancing) variant_args=(--no-gpu-instancing) ;;
        no_tessellation) variant_args=(--no-tessellation) ;;
        no_contact_shadows) variant_args=(--no-contact-shadows) ;;
        no_ambient_occlusion) variant_args=(--no-ambient-occlusion) ;;
        no_msaa) variant_args=(--no-msaa) ;;
        sun_shadow_2048) variant_args=(--sun-shadow-map 2048) ;;
        half_resolution)
            read -r width height < <(python3 - "$SNAPSHOT" <<'PY'
import json, sys
c = json.load(open(sys.argv[1]))
print(max(1, c['width']//2), max(1, c['height']//2))
PY
)
            variant_args=(--width "$width" --height "$height") ;;
    esac
    directory="$RUN_DIR/$name"
    mkdir -p "$directory"
    # Keep wrapper flags (GPU, WSI, binary path) while the final config snapshot
    # and variant flags make effective rendering settings repeatable.
    command=("$SCRIPT_DIR/run.sh" "${COMMON_ARGS[@]}" --config "$SNAPSHOT" "${variant_args[@]}")
    printf '%q ' "${command[@]}" > "$directory/command.sh"
    printf '\n' >> "$directory/command.sh"
    echo "==> $name: $directory"
    status=0
    HEAVEN_BENCHMARK_EXIT=1 HEAVEN_USER_DIR="$directory/user" \
        timeout "$PROFILE_TIMEOUT" "${command[@]}" 2>&1 | tee "$directory/run.log" || status=$?
    printf '%s\n' "$status" > "$directory/exit-status.txt"
    [[ "$status" == 0 ]] || exit "$status"
    python3 - "$directory" <<'PY'
from pathlib import Path
import json, sys
out = Path(sys.argv[1]); paths = list((out/'user/results').glob('*.json'))
if len(paths) != 1:
    raise SystemExit('Expected exactly one completed benchmark report')
r = json.loads(paths[0].read_text())
summary = {'report': str(paths[0]), 'frames': r['run']['frames'],
    'seconds': r['run']['duration_seconds'], 'api': r['settings']['render_api'],
    'fps': r['results']['average_fps'], 'gpu_ms': r['gpu']['timing_ms']['average'],
    'cpu_scene_meshes': r['cpu']['scene_meshes'], 'cpu_scene_instances': r['cpu']['scene_instances']}
(out/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
print(json.dumps(summary))
PY
done
echo "Profile results: $RUN_DIR"
