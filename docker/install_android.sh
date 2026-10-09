#!/usr/bin/env bash
# Install the tested Linux x86-64 Android toolchain inside a Docker build stage.
# SDK license acceptance matches the existing Android CI installation flow and
# applies only to the exact packages requested below, not an SDK-wide update.
set -euo pipefail

[[ $# -eq 1 && -f "$1" ]] || {
    echo "Usage: docker/install_android.sh android/versions.properties" >&2
    exit 2
}
[[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 ]] || {
    echo "The pinned Android build-host toolchain requires Linux x86-64." >&2
    exit 2
}
VERSIONS_FILE="$(realpath "$1")"
export JAVA_HOME=/opt/java17
export ANDROID_HOME=/opt/android-sdk
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/cmdline-tools/19.0/bin:$PATH"

JDK_URL='https://github.com/adoptium/temurin17-binaries/releases/download/jdk-17.0.20.1%2B1/OpenJDK17U-jdk_x64_linux_hotspot_17.0.20.1_1.tar.gz'
JDK_SHA256=3808d1d15e3ec6bd5b84057fb5d84c33d8a1536a258146bcea2e603fc726e08e
TOOLS_URL='https://dl.google.com/android/repository/commandlinetools-linux-13114758_latest.zip'
TOOLS_SHA256=7ec965280a073311c339e571cd5de778b9975026cfcbe79f2b1cdcb1e15317ee

work_dir="$(mktemp -d)"
trap 'rm -rf -- "$work_dir"' EXIT
download() {
    local url="$1" destination="$2" digest="$3"
    curl --fail --location --proto '=https' --proto-redir '=https' \
        --retry 3 --connect-timeout 30 --output "$destination" "$url"
    printf '%s  %s\n' "$digest" "$destination" | sha256sum --check --status
}

# Fail before downloads or installation if the shared package pins are invalid.
python3 - "$VERSIONS_FILE" "$work_dir/packages.txt" <<'PY'
from pathlib import Path
import re
import sys

values = dict(line.split('=', 1) for line in Path(sys.argv[1]).read_text().splitlines()
              if line and not line.startswith('#'))
for key, pattern in [('sdk', r'[1-9][0-9]*'),
                     ('buildTools', r'[0-9]+\.[0-9]+\.[0-9]+'),
                     ('ndk', r'[0-9]+\.[0-9]+\.[0-9]+')]:
    if not re.fullmatch(pattern, values.get(key, '')):
        raise SystemExit(f'Invalid or missing Android version pin: {key}')
Path(sys.argv[2]).write_text('\n'.join([
    'platforms;android-' + values['sdk'],
    'build-tools;' + values['buildTools'],
    'ndk;' + values['ndk'],
]) + '\n')
PY

download "$JDK_URL" "$work_dir/jdk.tar.gz" "$JDK_SHA256"
download "$TOOLS_URL" "$work_dir/commandlinetools.zip" "$TOOLS_SHA256"
mkdir -p "$JAVA_HOME" "$ANDROID_HOME/cmdline-tools"
tar --extract --gzip --file "$work_dir/jdk.tar.gz" \
    --directory "$JAVA_HOME" --strip-components=1 --no-same-owner
unzip -q "$work_dir/commandlinetools.zip" -d "$work_dir/tools"
mv "$work_dir/tools/cmdline-tools" "$ANDROID_HOME/cmdline-tools/19.0"

# A finite input file avoids masking sdkmanager failures with a SIGPIPE from
# `yes` under pipefail. Only these requested stable SDK packages are installed.
python3 - "$work_dir/license-input" <<'PY'
from pathlib import Path
import sys
Path(sys.argv[1]).write_text('y\n' * 100)
PY
sdkmanager --sdk_root="$ANDROID_HOME" --channel=0 \
    --package_file="$work_dir/packages.txt" < "$work_dir/license-input"

python3 - "$VERSIONS_FILE" "$JDK_URL" "$JDK_SHA256" "$TOOLS_URL" "$TOOLS_SHA256" <<'PY'
from pathlib import Path
import json
import sys

def properties(path):
    return {key.strip(): value.strip()
            for line in Path(path).read_text().splitlines()
            if line and not line.startswith('#') and '=' in line
            for key, value in [line.split('=', 1)]}

versions = properties(sys.argv[1])
root = Path('/opt/android-sdk')
components = {
    'command_line_tools': properties(root / 'cmdline-tools/19.0/source.properties'),
    'platform': properties(root / f"platforms/android-{versions['sdk']}/source.properties"),
    'build_tools': properties(root / f"build-tools/{versions['buildTools']}/source.properties"),
    'ndk': properties(root / f"ndk/{versions['ndk']}/source.properties"),
}
expected = [
    (components['command_line_tools'], 'Pkg.Revision', '19.0'),
    (components['platform'], 'AndroidVersion.ApiLevel', versions['sdk']),
    (components['build_tools'], 'Pkg.Revision', versions['buildTools']),
    (components['ndk'], 'Pkg.Revision', versions['ndk']),
    (properties('/opt/java17/release'), 'JAVA_VERSION', '"17.0.20.1"'),
]
for actual, key, value in expected:
    if actual.get(key, '').strip() != value:
        raise SystemExit(f'Installed toolchain mismatch: {key} expected {value}')
Path('/opt/android-toolchain.json').write_text(json.dumps({
    'jdk_url': sys.argv[2], 'jdk_sha256': sys.argv[3],
    'command_line_tools_url': sys.argv[4], 'command_line_tools_sha256': sys.argv[5],
    'requested_versions': versions, 'installed_components': components,
}, indent=2) + '\n')
PY
chmod -R a+rX "$JAVA_HOME" "$ANDROID_HOME"
java -version
printf '%s\n' 'Verified pinned Android SDK, NDK and JDK; receipt: /opt/android-toolchain.json'
