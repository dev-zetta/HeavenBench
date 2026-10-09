# syntax=docker/dockerfile:1
ARG UBUNTU_IMAGE=ubuntu:24.04@sha256:561618e2c15bf2397621dd04f96926663a3b5616c189cf7e38db7e82f5c538ea
FROM ${UBUNTU_IMAGE} AS toolchain
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
# Keep distro copyright/common-license files: packaging collects them.
RUN apt-get -o Acquire::Retries=3 update && apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
        ca-certificates curl gnupg git binutils coreutils tar gzip xz-utils unzip \
    && curl --fail --location --silent --show-error \
        --retry 4 --retry-all-errors --retry-delay 3 --retry-max-time 180 \
        --connect-timeout 15 --max-time 30 \
        --output /tmp/apt.llvm.org.gpg.key https://apt.llvm.org/llvm-snapshot.gpg.key \
    && gpg --dearmor -o /usr/share/keyrings/apt.llvm.org.gpg /tmp/apt.llvm.org.gpg.key \
    && rm /tmp/apt.llvm.org.gpg.key \
    && echo 'deb [signed-by=/usr/share/keyrings/apt.llvm.org.gpg] https://apt.llvm.org/noble/ llvm-toolchain-noble-21 main' \
        > /etc/apt/sources.list.d/llvm-21.list \
    && apt-get -o Acquire::Retries=3 update && apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
        clang-21 clang-tidy-21 cmake desktop-file-utils file libc++-21-dev libc++abi-21-dev \
        libglm-dev libglu1-mesa-dev libsdl2-dev libvulkan-dev libwayland-dev libx11-dev \
        libx11-xcb-dev libxcb1-dev libxcomposite-dev libxext-dev libxfixes-dev libxi-dev \
        libxrandr-dev libxxf86vm-dev ninja-build patchelf pkg-config python3 python3-yaml \
        ripgrep zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*
ENV HEAVEN_C_COMPILER=/usr/bin/clang-21 HEAVEN_CXX_COMPILER=/usr/bin/clang++-21 \
    CC=/usr/bin/clang-21 CXX=/usr/bin/clang++-21 \
    HEAVEN_CLANG_TIDY=/usr/bin/clang-tidy-21 HEAVEN_RUN_CLANG_TIDY=/usr/bin/run-clang-tidy-21 \
    HEAVEN_BUILD_ROOT=/workspace/build
WORKDIR /workspace

# Only pin/patch changes invalidate this download layer. No local vendor tree
# can leak into the build context or supply an undeclared dependency.
FROM toolchain AS dependencies
COPY scripts/fetch_dependencies.sh scripts/fetch_dependencies.sh
COPY patches/ patches/
RUN ./scripts/fetch_dependencies.sh

FROM toolchain AS android-toolchain
COPY android/versions.properties android/versions.properties
COPY docker/install_android.sh docker/install_android.sh
RUN ./docker/install_android.sh android/versions.properties
ENV JAVA_HOME=/opt/java17 ANDROID_HOME=/opt/android-sdk ANDROID_SDK_ROOT=/opt/android-sdk \
    GRADLE_USER_HOME=/workspace/build/android-gradle-cache ANDROID_USER_HOME=/workspace/build/android-user
ENV PATH="/opt/java17/bin:/opt/android-sdk/cmdline-tools/19.0/bin:${PATH}"

FROM dependencies AS linux-build
COPY . .
ARG CPU_ARCH=portable
ARG JOBS=2
ARG CACHE_SCOPE=default
ARG RUN_ANALYSIS=1
# One locked cache per architecture prevents CMake/compiler and downloader races.
RUN --mount=type=cache,id=heaven-linux-clang21-${CPU_ARCH}-${CACHE_SCOPE},target=/workspace/build,sharing=locked \
    --mount=type=bind,source=.,target=/source,readonly \
    set -e; \
    python3 scripts/prepare_docker_cache.py --source-root /source --platform linux --target "$CPU_ARCH"; \
    args=(); if [[ "$RUN_ANALYSIS" == 0 ]]; then args+=(--skip-analysis); fi; \
    ./scripts/ci_linux.sh --cpu-arch "$CPU_ARCH" --jobs "$JOBS" --output-dir /artifacts "${args[@]}" \
    && cp /workspace/build/.heaven-docker-cache.json /artifacts/logs/cache-state.json \
    && python3 scripts/export_build_sources.py --source-root /source --output-dir /artifacts --platform "linux-$CPU_ARCH"

# Exercise relocation without the compiler, libc++, SDL or development packages
# installed. System graphics drivers and ALSA remain host prerequisites.
FROM ${UBUNTU_IMAGE} AS linux-runtime-check
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
RUN apt-get -o Acquire::Retries=3 update && apt-get -o Acquire::Retries=3 install -y --no-install-recommends python3 libasound2t64 \
        libx11-6 libxext6 libxrender1 libxrandr2 libxi6 libxcursor1 libxfixes3 libxss1 \
        libwayland-client0 libwayland-cursor0 libwayland-egl1 libxkbcommon0 \
        libvulkan1 libgl1 libegl1 \
    && rm -rf /var/lib/apt/lists/*
COPY scripts/verify_appimage.py /verify_appimage.py
COPY --from=linux-build /artifacts/ /artifacts/
ARG CPU_ARCH=portable
RUN if [[ "$CPU_ARCH" == portable ]]; then \
        python3 /verify_appimage.py /artifacts/*portable.AppImage 2>&1 | tee /artifacts/logs/runtime-verification.log; \
    else echo 'Architecture-specific executable not run on a generic build host.' > /artifacts/logs/runtime-verification.log; fi

FROM android-toolchain AS android-build
COPY --from=dependencies /workspace/lib/ /workspace/lib/
COPY . .
ARG ANDROID_ABI=arm64-v8a
ARG JOBS=2
ARG CACHE_SCOPE=default
RUN --mount=type=cache,id=heaven-android-${ANDROID_ABI}-${CACHE_SCOPE},target=/workspace/build,sharing=locked \
    --mount=type=bind,source=.,target=/source,readonly \
    python3 scripts/prepare_docker_cache.py --source-root /source --platform android --target "$ANDROID_ABI" \
    && ./scripts/ci_android.sh --abi "$ANDROID_ABI" --jobs "$JOBS" --output-dir /artifacts \
    && cp /workspace/build/.heaven-docker-cache.json /artifacts/logs/cache-state.json \
    && python3 scripts/export_build_sources.py --source-root /source --output-dir /artifacts --platform "android-$ANDROID_ABI"

FROM scratch AS linux-artifacts
COPY --from=linux-runtime-check /artifacts/ /

FROM scratch AS android-artifacts
COPY --from=android-build /artifacts/ /
