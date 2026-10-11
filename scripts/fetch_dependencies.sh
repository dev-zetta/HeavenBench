#!/usr/bin/env bash
# Fetch the exact third-party source revisions used by HeavenBench.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." &>/dev/null && pwd)"

FILAMENT_URL="https://github.com/google/filament.git"
FILAMENT_UPSTREAM_REV="1da46b0a2940db9b1286ec3a8910954c191c010c"
FILAMENT_PATCH_SHA256="9241e94deca991f847dab7c42e8c9557d402c581082560c9f04b24ba3d7f3de8"
FILAMENT_EXPECTED_TREE="6bcb1972730792dce720d9e329f20ca3441c2a38"
FILAMENT_PATCH="$REPO_ROOT/patches/filament/heavenbench.patch"
JOLT_REV="9e17b843a1e1870f9e6c248c836e700b1253f010"
RAPIDCHECK_REV="b2d9ed2dddefc4b84318d664b4f221eb792d89c7"
LIBZIP_REV="6f8a0cdd24a0dc6cce9dac4a7679da784ab124ea"
POCKETFFT_REV="c90e55b3d529f8efa40ed01a20de22405f45fc65"
BCDEC_REV="93628fe5627102fe5187b7eeb99122dec6612c36"

fetch_revision() {
    local name="$1"
    local url="$2"
    local revision="$3"
    local destination="$4"

    if [[ -e "$destination/.git" ]]; then
        local current_revision
        if current_revision="$(git -C "$destination" rev-parse --verify HEAD 2>/dev/null)"; then
            if [[ "$current_revision" != "$revision" ]]; then
                echo "Error: $name is at $current_revision; expected $revision" >&2
                exit 1
            fi
            if ! git -C "$destination" diff --quiet -- ||
               ! git -C "$destination" diff --cached --quiet --; then
                echo "Error: $name has tracked changes; preserve or revert them before fetching pinned sources" >&2
                exit 1
            fi
            echo "==> Using pinned $name checkout"
            return
        fi

        # A failed initial fetch leaves a valid repository with no HEAD. Retry
        # only when its worktree and index are empty; never overwrite partial
        # files or local work while recovering an interrupted bootstrap.
        local pending_changes
        pending_changes="$(git -C "$destination" status --porcelain --untracked-files=all --ignored)"
        if [[ -n "$pending_changes" ]]; then
            echo "Error: incomplete $name checkout contains local files; preserve them before retrying" >&2
            exit 1
        fi
        echo "==> Resuming interrupted $name fetch"
    elif [[ -e "$destination" ]]; then
        echo "Error: $destination exists but is not a Git checkout" >&2
        exit 1
    else
        git init --quiet "$destination"
        git -C "$destination" remote add origin "$url"
    fi

    echo "==> Fetching $name at $revision"
    git -C "$destination" fetch --quiet --depth 1 "$url" "$revision"
    git -C "$destination" checkout --quiet --no-overwrite-ignore --detach FETCH_HEAD
}

fetch_filament() (
    local destination="$REPO_ROOT/lib/filament"
    local current_revision current_tree pending_changes
    if [[ ! "$FILAMENT_UPSTREAM_REV" =~ ^[0-9a-f]{40}$ ||
          ! "$FILAMENT_PATCH_SHA256" =~ ^[0-9a-f]{64}$ ||
          ! "$FILAMENT_EXPECTED_TREE" =~ ^[0-9a-f]{40}$ ]]; then
        echo "Error: incomplete Filament upstream/patch/tree pins" >&2
        exit 1
    fi
    # The patch is part of this repository's source archive. Never fetch an
    # unpublished fork commit or accept a patch changed without its audited pin.
    local actual_patch_sha256
    if command -v sha256sum >/dev/null 2>&1; then
        actual_patch_sha256="$(sha256sum "$FILAMENT_PATCH")"
    else
        # macOS supplies shasum instead of the GNU coreutils executable.
        actual_patch_sha256="$(shasum -a 256 "$FILAMENT_PATCH")"
    fi
    [[ "${actual_patch_sha256%% *}" == "$FILAMENT_PATCH_SHA256" ]] || {
            echo "Error: Filament patch is missing or its SHA-256 does not match" >&2
            exit 1
        }
    if [[ -e "$destination/.git" ]]; then
        if current_revision="$(git -C "$destination" rev-parse --verify HEAD 2>/dev/null)"; then
            if ! git -C "$destination" diff --quiet -- ||
               ! git -C "$destination" diff --cached --quiet --; then
                echo "Error: Filament has tracked changes; preserve them before fetching pinned sources" >&2
                exit 1
            fi
            current_tree="$(git -C "$destination" rev-parse 'HEAD^{tree}')"
            if [[ "$current_tree" == "$FILAMENT_EXPECTED_TREE" ]]; then
                echo "==> Using pinned Filament source tree"
                return
            fi
            if [[ "$current_revision" != "$FILAMENT_UPSTREAM_REV" ]]; then
                echo "Error: Filament is at $current_revision with source tree $current_tree" >&2
                echo "Expected upstream $FILAMENT_UPSTREAM_REV or patched tree $FILAMENT_EXPECTED_TREE; preserve the checkout and use a fresh dependency directory" >&2
                exit 1
            fi
        else
            pending_changes="$(git -C "$destination" status --porcelain --untracked-files=all --ignored)"
            if [[ -n "$pending_changes" ]]; then
                echo "Error: incomplete Filament checkout contains local files; preserve them before retrying" >&2
                exit 1
            fi
            echo "==> Resuming interrupted Filament fetch"
        fi
    elif [[ -e "$destination" ]]; then
        echo "Error: $destination exists but is not a Git checkout" >&2
        exit 1
    else
        git init --quiet "$destination"
        git -C "$destination" remote add origin "$FILAMENT_URL"
    fi

    if [[ -z "${current_revision:-}" ]]; then
        echo "==> Fetching Filament upstream at $FILAMENT_UPSTREAM_REV"
        git -C "$destination" fetch --quiet --depth 1 "$FILAMENT_URL" "$FILAMENT_UPSTREAM_REV"
        git -C "$destination" checkout --quiet --no-overwrite-ignore --detach "$FILAMENT_UPSTREAM_REV"
    fi

    # Construct and verify the patched tree in an isolated index. A bad patch,
    # tree mismatch or interrupted preparation cannot modify the real index or
    # tracked worktree. Explicitly protect ignored files too: ordinary checkout
    # overwrites them even without --force when the new tree adds their paths.
    local temporary_directory patched_tree patched_commit
    temporary_directory="$(mktemp -d)"
    trap 'rm -rf -- "$temporary_directory"' EXIT
    local temporary_index="$temporary_directory/index"
    GIT_INDEX_FILE="$temporary_index" git -C "$destination" read-tree "$FILAMENT_UPSTREAM_REV"
    GIT_INDEX_FILE="$temporary_index" git -C "$destination" apply --cached --whitespace=nowarn "$FILAMENT_PATCH"
    patched_tree="$(GIT_INDEX_FILE="$temporary_index" git -C "$destination" write-tree)"
    if [[ "$patched_tree" != "$FILAMENT_EXPECTED_TREE" ]]; then
        echo "Error: Filament patch produced source tree $patched_tree; expected $FILAMENT_EXPECTED_TREE" >&2
        exit 1
    fi
    # Stable metadata gives every fresh bootstrap the same local commit ID.
    # An existing rebased commit may have different metadata; the tree pin is
    # the authoritative source identity and is checked on every invocation.
    patched_commit="$(
        GIT_AUTHOR_NAME='HeavenBench dependency bootstrap' \
        GIT_AUTHOR_EMAIL='build@heavenbench.invalid' \
        GIT_COMMITTER_NAME='HeavenBench dependency bootstrap' \
        GIT_COMMITTER_EMAIL='build@heavenbench.invalid' \
        GIT_AUTHOR_DATE='2000-01-01T00:00:00+0000' \
        GIT_COMMITTER_DATE='2000-01-01T00:00:00+0000' \
            git -C "$destination" -c commit.gpgsign=false -c i18n.commitEncoding=UTF-8 commit-tree \
                "$patched_tree" -p "$FILAMENT_UPSTREAM_REV" \
                -m "Apply pinned HeavenBench Filament rendering extensions"
    )"
    git -C "$destination" checkout --quiet --no-overwrite-ignore --detach "$patched_commit"
    echo "==> Filament upstream and HeavenBench patch verified"
)

fetch_filament

fetch_revision \
    "Jolt Physics" "https://github.com/jrouwe/JoltPhysics.git" \
    "$JOLT_REV" "$REPO_ROOT/lib/JoltPhysics"
fetch_revision \
    "RapidCheck" "https://github.com/emil-e/rapidcheck.git" \
    "$RAPIDCHECK_REV" "$REPO_ROOT/lib/rapidcheck"
fetch_revision \
    "libzip" "https://github.com/nih-at/libzip.git" \
    "$LIBZIP_REV" "$REPO_ROOT/lib/libzip"
fetch_revision \
    "pocketfft" "https://gitlab.mpcdf.mpg.de/mtr/pocketfft.git" \
    "$POCKETFFT_REV" "$REPO_ROOT/lib/pocketfft"
fetch_revision \
    "bcdec" "https://github.com/iOrange/bcdec.git" \
    "$BCDEC_REV" "$REPO_ROOT/lib/bcdec"

echo "==> Third-party dependencies are ready"
