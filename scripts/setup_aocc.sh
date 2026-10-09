#!/usr/bin/env bash
# Download and install AMD AOCC into HeavenBench's ignored build tree.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." &>/dev/null && pwd)"
# shellcheck source=build_paths.sh
source "$SCRIPT_DIR/build_paths.sh"

AOCC_VERSION="5.2.0"
AOCC_ARCHIVE_NAME="aocc-compiler-${AOCC_VERSION}.tar"
AOCC_ARCHIVE_URL="https://download.amd.com/developer/eula/aocc/aocc-5-2/${AOCC_ARCHIVE_NAME}"
AOCC_ARCHIVE_SHA256="f98af7e2ae8801dd4ba443520653acb739536a86c2a1caf096310c3cfd554ca0"
AOCC_EULA_URL="https://www.amd.com/en/developer/aocc/eula/aocc-5-2-eula.html"
AOCC_COMPILER_DIR="$HEAVEN_AOCC_ROOT/aocc-compiler-${AOCC_VERSION}"
AOCC_ENV_FILE="$HEAVEN_AOCC_ROOT/setenv_AOCC.sh"
ARCHIVE_PATH=""
ACCEPT_EULA=0
DOWNLOAD_TMP=""
STAGING_DIR=""
PROBE_DIR=""

cleanup() {
    if [[ -n "$DOWNLOAD_TMP" && -f "$DOWNLOAD_TMP" ]]; then
        rm -f -- "$DOWNLOAD_TMP"
    fi
    if [[ -n "$STAGING_DIR" && -d "$STAGING_DIR" ]]; then
        rm -rf -- "$STAGING_DIR"
    fi
    if [[ -n "$PROBE_DIR" && -d "$PROBE_DIR" ]]; then
        rm -rf -- "$PROBE_DIR"
    fi
}
trap cleanup EXIT

usage() {
    cat <<EOF
HeavenBench AOCC ${AOCC_VERSION} local setup

Usage: $0 --accept-eula [options]

Options:
  --accept-eula      Confirm that you have read and accept AMD's AOCC 5.2 EULA
  --archive FILE     Use an existing AOCC tar archive instead of downloading it
  --help, -h         Show this help

Install directory:
  $HEAVEN_AOCC_ROOT

EULA:
  $AOCC_EULA_URL

The compiler is installed locally under the ignored build/ tree. No sudo or
system package installation is used.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --accept-eula)
            ACCEPT_EULA=1
            shift ;;
        --archive)
            if [[ $# -lt 2 ]]; then
                echo "Error: --archive requires a file path" >&2
                exit 1
            fi
            ARCHIVE_PATH="$2"
            shift 2 ;;
        --help|-h)
            usage
            exit 0 ;;
        *)
            echo "Error: unknown argument '$1' (use --help for usage)" >&2
            exit 1 ;;
    esac
done

if [[ "$ACCEPT_EULA" -ne 1 ]]; then
    echo "Error: AOCC is distributed under AMD's End User License Agreement." >&2
    echo "Read $AOCC_EULA_URL" >&2
    echo "Rerun with --accept-eula only if you accept those terms." >&2
    exit 1
fi

if [[ -e "$HEAVEN_AOCC_ROOT" ]]; then
    if [[ ! -f "$AOCC_COMPILER_DIR/install.sh" ||
          ! -x "$AOCC_COMPILER_DIR/bin/clang" ||
          ! -x "$AOCC_COMPILER_DIR/bin/clang++" ]]; then
        echo "Error: incomplete AOCC extraction exists at $HEAVEN_AOCC_ROOT" >&2
        echo "Move it aside or remove it, then rerun this setup." >&2
        exit 1
    fi
    if [[ -f "$AOCC_ENV_FILE" ]]; then
        echo "==> Reusing AOCC ${AOCC_VERSION}: $HEAVEN_AOCC_ROOT"
    else
        echo "==> Repairing AOCC's generated environment"
    fi
else
    for tool in awk sha256sum tar; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "Error: required tool '$tool' was not found" >&2
            exit 1
        fi
    done

    if [[ -z "$ARCHIVE_PATH" ]]; then
        if ! command -v curl >/dev/null 2>&1; then
            echo "Error: curl is required to download AOCC" >&2
            exit 1
        fi

        download_dir="$HEAVEN_BUILD_ROOT/downloads"
        ARCHIVE_PATH="$download_dir/$AOCC_ARCHIVE_NAME"
        mkdir -p "$download_dir"

        if [[ ! -f "$ARCHIVE_PATH" ]]; then
            DOWNLOAD_TMP="${ARCHIVE_PATH}.part.$$"
            echo "==> Downloading AOCC ${AOCC_VERSION}"
            curl --fail --location --output "$DOWNLOAD_TMP" "$AOCC_ARCHIVE_URL"
            mv -- "$DOWNLOAD_TMP" "$ARCHIVE_PATH"
            DOWNLOAD_TMP=""
        else
            echo "==> Using cached archive: $ARCHIVE_PATH"
        fi
    elif [[ ! -f "$ARCHIVE_PATH" ]]; then
        echo "Error: AOCC archive not found: $ARCHIVE_PATH" >&2
        exit 1
    fi

    actual_sha256="$(sha256sum "$ARCHIVE_PATH" | awk '{print $1}')"
    if [[ "$actual_sha256" != "$AOCC_ARCHIVE_SHA256" ]]; then
        echo "Error: AOCC archive checksum mismatch" >&2
        echo "Expected: $AOCC_ARCHIVE_SHA256" >&2
        echo "Actual:   $actual_sha256" >&2
        echo "Remove or replace the invalid archive: $ARCHIVE_PATH" >&2
        exit 1
    fi
    echo "==> AOCC archive checksum verified"

    aocc_parent="$(dirname -- "$HEAVEN_AOCC_ROOT")"
    mkdir -p "$aocc_parent"
    STAGING_DIR="$(mktemp -d "$aocc_parent/.aocc-5.2.0.XXXXXX")"
    echo "==> Extracting AOCC to a temporary directory"
    tar -xf "$ARCHIVE_PATH" -C "$STAGING_DIR"

    if [[ ! -f "$STAGING_DIR/aocc-compiler-${AOCC_VERSION}/install.sh" ]]; then
        echo "Error: archive did not contain the expected AOCC installer" >&2
        exit 1
    fi

    mv -- "$STAGING_DIR" "$HEAVEN_AOCC_ROOT"
    STAGING_DIR=""
fi

if [[ ! -f "$AOCC_ENV_FILE" ]]; then
    echo "==> Generating the AOCC environment"
    if ! (cd "$AOCC_COMPILER_DIR" && bash install.sh); then
        echo "Error: AOCC's installer reported a missing prerequisite" >&2
        echo "The local extraction remains at $HEAVEN_AOCC_ROOT for inspection." >&2
        exit 1
    fi
fi

if [[ ! -f "$AOCC_ENV_FILE" ||
      ! -x "$AOCC_COMPILER_DIR/bin/clang" ||
      ! -x "$AOCC_COMPILER_DIR/bin/clang++" ]]; then
    echo "Error: AOCC setup completed without the expected compiler files" >&2
    exit 1
fi

PROBE_DIR="$(mktemp -d)"
probe_source="$PROBE_DIR/aocc_probe.cpp"
probe_binary="$PROBE_DIR/aocc_probe"
printf '%s\n' \
    '#include <expected>' \
    'int main() {' \
    '    std::expected<int, int> value(0);' \
    '    return *value;' \
    '}' > "$probe_source"

if ! (
    set +u
    # shellcheck disable=SC1090
    source "$AOCC_ENV_FILE"
    "$AOCC_COMPILER_DIR/bin/clang++" \
        -std=c++23 -stdlib=libc++ \
        "$probe_source" -lc++abi -o "$probe_binary"
    "$probe_binary"
); then
    echo "Error: AOCC failed the C++23/libc++ compile-link-run probe" >&2
    echo "Review the prerequisite diagnostics above." >&2
    exit 1
fi

echo "==> AOCC ${AOCC_VERSION} ready: $HEAVEN_AOCC_ROOT"
echo "==> Build with: ./scripts/build.sh --compiler aocc --cpu-arch zen3"
