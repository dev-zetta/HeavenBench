#!/usr/bin/env bash
# Build the SDL Android shared libraries and an installable development APK.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
# shellcheck source=build_paths.sh
source "$SCRIPT_DIR/build_paths.sh"
HEAVEN_BUILD_ROOT="$(realpath -m "$HEAVEN_BUILD_ROOT")"

ABI=arm64-v8a
JOBS="${HEAVEN_JOBS:-$(nproc)}"
HOST_BUILD_DIR="$HEAVEN_BUILD_ROOT/android-host-renderer"
PACKAGE_ONLY=0
GRADLE_COMMAND="${HEAVEN_GRADLE:-}"

usage() {
    cat <<'EOF'
Usage: scripts/build_android.sh [--abi arm64-v8a|x86_64] [--jobs N]
       [--host-build-dir DIR] [--package-only]

Requires Linux, Java 17+, Android SDK API 35/build-tools 35.0.0, pinned NDK
28.2.13676358, and the renderer's normal Linux host-build prerequisites.
Set ANDROID_HOME (or ANDROID_SDK_ROOT). Native and Java SDL sources come from
the same checksum-pinned SDL2 release; SDL and Gradle are fetched into build/.
The APK is debug-signed for installation/testing, with optimized native code.
It contains no proprietary Heaven assets; import a desktop-produced assets.zip.

--abi ABI             APK/native ABI (default: arm64-v8a)
--jobs N              CMake parallelism
--host-build-dir DIR  Reuse a Linux host build for Filament generators
--package-only        Repackage already-built shared libraries

HEAVEN_GRADLE may select an installed Gradle 8.13 binary. HEAVEN_BUILD_ROOT
selects the common generated output root. This helper does not publish APKs,
install SDK components, or accept Android SDK license terms automatically.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --abi|--jobs|--host-build-dir)
            [[ $# -ge 2 ]] || { echo "Missing value for $1" >&2; exit 2; }
            case "$1" in
                --abi) ABI="$2" ;;
                --jobs) JOBS="$2" ;;
                --host-build-dir) HOST_BUILD_DIR="$2" ;;
            esac
            shift 2 ;;
        --package-only) PACKAGE_ONLY=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done
[[ "$ABI" == arm64-v8a || "$ABI" == x86_64 ]] || { echo "Unsupported ABI: $ABI" >&2; exit 2; }
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "--jobs must be a positive integer" >&2; exit 2; }
HOST_BUILD_DIR="$(realpath -m "$HOST_BUILD_DIR")"
[[ "$(uname -s)" == Linux ]] || { echo "This helper currently supports Linux build hosts." >&2; exit 2; }
for tool in python3 curl tar unzip sha256sum java cmake ninja; do
    command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 1; }
done

version() {
    python3 - "$REPO_ROOT/android/versions.properties" "$1" <<'PY'
import sys
from pathlib import Path
values = dict(line.split('=', 1) for line in Path(sys.argv[1]).read_text().splitlines()
              if line and not line.startswith('#'))
print(values[sys.argv[2]])
PY
}
NDK_VERSION="$(version ndk)"
SDK_VERSION="$(version sdk)"
BUILD_TOOLS_VERSION="$(version buildTools)"
MIN_SDK="$(version minSdk)"
SDL_VERSION="$(version sdl)"
GRADLE_VERSION="$(version gradle)"
SDK_DIR="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
[[ -n "$SDK_DIR" ]] || { echo "Set ANDROID_HOME to an installed Android SDK." >&2; exit 1; }
SDK_DIR="$(realpath "$SDK_DIR")"
export ANDROID_HOME="$SDK_DIR"
export ANDROID_SDK_ROOT="$SDK_DIR"
export ANDROID_NDK_HOME="$SDK_DIR/ndk/$NDK_VERSION"
for required in "$SDK_DIR/platforms/android-$SDK_VERSION/android.jar" \
                "$SDK_DIR/build-tools/$BUILD_TOOLS_VERSION/apksigner" \
                "$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake"; do
    [[ -f "$required" ]] || {
        echo "Missing Android SDK component: $required" >&2
        echo "Install with sdkmanager 'platforms;android-$SDK_VERSION' 'build-tools;$BUILD_TOOLS_VERSION' 'ndk;$NDK_VERSION'." >&2
        exit 1
    }
done

DOWNLOADS="$HEAVEN_BUILD_ROOT/android-downloads"
mkdir -p "$DOWNLOADS" "$HEAVEN_BUILD_ROOT/dependencies"
download() {
    local url="$1" destination="$2" digest="$3"
    if [[ ! -f "$destination" ]]; then
        curl --fail --location --retry 3 --output "$destination.partial" "$url"
        printf '%s  %s\n' "$digest" "$destination.partial" | sha256sum --check --status
        mv "$destination.partial" "$destination"
    fi
    printf '%s  %s\n' "$digest" "$destination" | sha256sum --check --status
}

SDL_ARCHIVE="$DOWNLOADS/SDL2-$SDL_VERSION.tar.gz"
download "https://www.libsdl.org/release/SDL2-$SDL_VERSION.tar.gz" "$SDL_ARCHIVE" "$(version sdlSha256)"
SDL_SOURCE="$HEAVEN_BUILD_ROOT/dependencies/SDL2-$SDL_VERSION"
if [[ ! -d "$SDL_SOURCE" ]]; then
    tar -xf "$SDL_ARCHIVE" -C "$HEAVEN_BUILD_ROOT/dependencies"
fi
python3 "$REPO_ROOT/android/test_sdl_java.py" "$SDL_SOURCE"
python3 "$REPO_ROOT/android/test_native_bridge.py"
if [[ -z "$GRADLE_COMMAND" ]]; then
    GRADLE_ARCHIVE="$DOWNLOADS/gradle-$GRADLE_VERSION-bin.zip"
    download "https://services.gradle.org/distributions/gradle-$GRADLE_VERSION-bin.zip" \
        "$GRADLE_ARCHIVE" "$(version gradleSha256)"
    if [[ ! -x "$HEAVEN_BUILD_ROOT/dependencies/gradle-$GRADLE_VERSION/bin/gradle" ]]; then
        unzip -q "$GRADLE_ARCHIVE" -d "$HEAVEN_BUILD_ROOT/dependencies"
    fi
    GRADLE_COMMAND="$HEAVEN_BUILD_ROOT/dependencies/gradle-$GRADLE_VERSION/bin/gradle"
fi
export GRADLE_USER_HOME="${GRADLE_USER_HOME:-$HEAVEN_BUILD_ROOT/android-gradle-cache}"

NATIVE_BUILD_DIR="$(heaven_renderer_build_dir android Release "$ABI" clang)"
if [[ "$PACKAGE_ONLY" -eq 0 ]]; then
    IMPORT_DIRECTORY="$HEAVEN_BUILD_ROOT/android-host-tools"
    mkdir -p "$IMPORT_DIRECTORY"
    # Filament prefixes this value with its source directory, so it must be a
    # relative path even when HEAVEN_BUILD_ROOT is outside the checkout.
    IMPORT_RELATIVE="$(python3 - "$IMPORT_DIRECTORY" "$REPO_ROOT/lib/filament" <<'PY'
import os, sys
print(os.path.relpath(sys.argv[1], sys.argv[2]))
PY
)"
    "$SCRIPT_DIR/build.sh" --platform linux --release --jobs "$JOBS" \
        --build-dir "$HOST_BUILD_DIR" \
        --cmake-arg "-DIMPORT_EXECUTABLES_DIR=$IMPORT_RELATIVE" \
        --target matc --target cmgen --target filamesh --target mipgen \
        --target resgen --target uberz --target glslminifier
    "$SCRIPT_DIR/build.sh" --platform android --release --jobs "$JOBS" \
        --android-abi "$ABI" --android-api "$MIN_SDK" \
        --build-dir "$NATIVE_BUILD_DIR" \
        --cmake-arg "-DIMPORT_EXECUTABLES_DIR=$IMPORT_RELATIVE" \
        --cmake-arg "-DHEAVEN_SDL_SOURCE_DIR=$SDL_SOURCE" \
        --cmake-arg "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
fi

STAGING="$HEAVEN_BUILD_ROOT/android-package/$ABI"
JNI_DIR="$STAGING/jniLibs"
ASSET_DIR="$STAGING/assets"
mkdir -p "$JNI_DIR/$ABI" "$ASSET_DIR"
cp "$NATIVE_BUILD_DIR/libheaven_benchmark.so" "$JNI_DIR/$ABI/"
cp "$NATIVE_BUILD_DIR/sdl2/libSDL2.so" "$JNI_DIR/$ABI/"
python3 "$REPO_ROOT/android/check_native.py" --directory "$JNI_DIR/$ABI" --abi "$ABI" \
    --tools "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
cp -a "$REPO_ROOT/benchmark/assets/." "$ASSET_DIR/resources/"
python3 "$SCRIPT_DIR/collect_licenses.py" --output "$ASSET_DIR/licenses" \
    --dependency-dir "SDL2=$SDL_SOURCE" \
    --dependency-dir "GLM=${HEAVEN_GLM_LICENSE_DIR:-/usr/share/doc/libglm-dev}" \
    --dependency-dir "NDK=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64"

VERSION="$(python3 - "$REPO_ROOT/benchmark/CMakeLists.txt" <<'PY'
import pathlib, re, sys
print(re.search(r'project\(HeavenBench VERSION ([0-9.]+)', pathlib.Path(sys.argv[1]).read_text())[1])
PY
)"
GRADLE_OUTPUT="$HEAVEN_BUILD_ROOT/android-gradle/$ABI"
"$GRADLE_COMMAND" --no-daemon --console=plain -p "$REPO_ROOT/android" \
    "-PheavenBuildRoot=$GRADLE_OUTPUT" "-PheavenJniDir=$JNI_DIR" \
    "-PheavenAssetsDir=$ASSET_DIR" "-PheavenSdlSource=$SDL_SOURCE" \
    "-PheavenAbi=$ABI" "-PheavenVersion=$VERSION" :app:lintDebug :app:assembleDebug

APK="$GRADLE_OUTPUT/app/outputs/apk/debug/app-debug.apk"
"$SDK_DIR/build-tools/$BUILD_TOOLS_VERSION/apksigner" verify "$APK"
"$SDK_DIR/build-tools/$BUILD_TOOLS_VERSION/zipalign" -c -P 16 4 "$APK"
python3 - "$APK" "$ABI" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as archive:
    names = set(archive.namelist())
    required = {'AndroidManifest.xml', 'classes.dex', 'assets/licenses/MANIFEST.json',
                f'lib/{sys.argv[2]}/libSDL2.so', f'lib/{sys.argv[2]}/libheaven_benchmark.so'}
    missing = required - names
    if missing:
        raise SystemExit(f'Incomplete APK: missing {sorted(missing)}')
PY
DESTINATION="$HEAVEN_BUILD_ROOT/packages/HeavenBench-$VERSION-android-$ABI-debug.apk"
mkdir -p "$(dirname "$DESTINATION")"
cp "$APK" "$DESTINATION"
(cd "$(dirname "$DESTINATION")" && sha256sum "$(basename "$DESTINATION")") > "$DESTINATION.sha256"
echo "==> Installable development APK: $DESTINATION"
