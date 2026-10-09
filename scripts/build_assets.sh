#!/usr/bin/env bash
# Build a runtime asset ZIP from the pinned official installer (Python 3.10+).
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
# shellcheck source=build_paths.sh
source "$SCRIPT_DIR/build_paths.sh"
export HEAVEN_BUILD_ROOT
exec python3 "$REPO_ROOT/tools/asset_builder/build_assets.py" "$@"
