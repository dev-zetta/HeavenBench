cmake_minimum_required(VERSION 3.25)
# C is enabled (alongside CXX) so the vendored single-file C decoder
# third_party/stb_vorbis.c compiles with the C toolchain, mirroring the
# top-level project's bundled lz4.c precedent. Our own renderer code is all
# C++ (the standard is set per-target below); enabling C does not change it.
project(HeavenBench VERSION 1.0.1 LANGUAGES C CXX)

# Note: C++ standard is set per-target below, not globally,
# to avoid leaking C++23 into Filament's subdirectory build.
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HeavenProjectOptions.cmake)

set(HEAVEN_TARGET_PLATFORM "auto" CACHE STRING "Target platform: auto, linux, windows, macos, or android")
set_property(CACHE HEAVEN_TARGET_PLATFORM PROPERTY STRINGS auto linux windows macos android)
string(TOLOWER "${HEAVEN_TARGET_PLATFORM}" HEAVEN_TARGET_PLATFORM_LOWER)
if(HEAVEN_TARGET_PLATFORM_LOWER STREQUAL "auto")
    if(ANDROID)
        set(HEAVEN_PLATFORM_ANDROID ON)
        set(HEAVEN_TARGET_PLATFORM_LOWER android)
    elseif(WIN32)
        set(HEAVEN_PLATFORM_WINDOWS ON)
        set(HEAVEN_TARGET_PLATFORM_LOWER windows)
    elseif(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(HEAVEN_PLATFORM_MACOS ON)
        set(HEAVEN_TARGET_PLATFORM_LOWER macos)
    elseif(UNIX AND NOT APPLE)
        set(HEAVEN_PLATFORM_LINUX ON)
        set(HEAVEN_TARGET_PLATFORM_LOWER linux)
    else()
        message(FATAL_ERROR "Unsupported auto-detected platform; set HEAVEN_TARGET_PLATFORM explicitly")
    endif()
elseif(HEAVEN_TARGET_PLATFORM_LOWER STREQUAL "android")
    set(HEAVEN_PLATFORM_ANDROID ON)
    if(NOT ANDROID)
        message(FATAL_ERROR "HEAVEN_TARGET_PLATFORM=android requires an Android toolchain")
    endif()
elseif(HEAVEN_TARGET_PLATFORM_LOWER STREQUAL "windows")
    set(HEAVEN_PLATFORM_WINDOWS ON)
    if(NOT WIN32)
        message(FATAL_ERROR "HEAVEN_TARGET_PLATFORM=windows requires a Windows toolchain")
    endif()
elseif(HEAVEN_TARGET_PLATFORM_LOWER STREQUAL "linux")
    set(HEAVEN_PLATFORM_LINUX ON)
    if(NOT (UNIX AND NOT APPLE AND NOT ANDROID))
        message(FATAL_ERROR "HEAVEN_TARGET_PLATFORM=linux requires a Linux toolchain")
    endif()
elseif(HEAVEN_TARGET_PLATFORM_LOWER STREQUAL "macos")
    set(HEAVEN_PLATFORM_MACOS ON)
    if(NOT (APPLE AND CMAKE_SYSTEM_NAME STREQUAL "Darwin"))
        message(FATAL_ERROR "HEAVEN_TARGET_PLATFORM=macos requires a macOS toolchain")
    endif()
else()
    message(FATAL_ERROR "Unsupported HEAVEN_TARGET_PLATFORM='${HEAVEN_TARGET_PLATFORM}'")
endif()
message(STATUS "Heaven target platform: ${HEAVEN_TARGET_PLATFORM_LOWER}")

if(HEAVEN_PLATFORM_MACOS)
    if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
        message(FATAL_ERROR "macOS builds require CMAKE_OSX_ARCHITECTURES=arm64 (Apple Silicon)")
    endif()
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET OR
       CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 14.0)
        message(FATAL_ERROR "macOS builds require a deployment target of 14.0 or newer")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
       CMAKE_CXX_COMPILER_VERSION VERSION_LESS 21)
        message(FATAL_ERROR "macOS builds require upstream LLVM Clang 21+ and its bundled libc++; use scripts/build.sh")
    endif()
    # Apple SDK libc++ can lag the C++23 API used by owned sources. Check a
    # linked program, not only the language flag, before configuring vendors.
    include(CheckCXXSourceCompiles)
    function(heaven_check_macos_cxx23)
        # try_compile appends the selected language standard after required
        # flags. Set its standard in this scope so C++17 cannot override the
        # probe, without leaking C++23 into Filament's directory.
        set(CMAKE_CXX_STANDARD 23)
        set(CMAKE_CXX_STANDARD_REQUIRED ON)
        unset(HEAVEN_MACOS_CXX23_SUPPORTED CACHE)
        check_cxx_source_compiles("
        #include <expected>
        #include <format>
        #if !defined(_LIBCPP_VERSION) || _LIBCPP_VERSION < 210000
        #error LLVM libc++ 21 or newer is required
        #endif
        int main() {
            std::expected<int, int> value = 42;
            return std::format(\"{}\", *value) != \"42\";
        }" HEAVEN_MACOS_CXX23_SUPPORTED)
    endfunction()
    heaven_check_macos_cxx23()
    if(NOT HEAVEN_MACOS_CXX23_SUPPORTED)
        message(FATAL_ERROR "macOS C++23 libc++ compile/link check failed; use scripts/build.sh with Homebrew llvm@21")
    endif()
    enable_language(OBJC OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 23)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)
endif()

# Architecture-specific variants deliberately default to portable. Like a
# distribution's optimized package, Zen-targeted binaries may execute instructions
# that older x86-64 processors do not support. This directory-scoped option is
# applied to the final executables and their runtime libraries. Filament's
# build-time generators stay portable because they execute on the build host,
# which may differ from the selected target (notably on hosted CI).
set(HEAVEN_CPU_ARCH "portable" CACHE STRING
    "CPU optimization target: portable, native, or zen1 through zen5")
set_property(CACHE HEAVEN_CPU_ARCH PROPERTY STRINGS
    portable native zen1 zen2 zen3 zen4 zen5)
string(TOLOWER "${HEAVEN_CPU_ARCH}" HEAVEN_CPU_ARCH_LOWER)

if(NOT HEAVEN_CPU_ARCH_LOWER MATCHES "^(portable|native|zen[1-5])$")
    message(FATAL_ERROR
        "Unsupported HEAVEN_CPU_ARCH='${HEAVEN_CPU_ARCH}'; "
        "expected portable, native, or zen1 through zen5")
endif()

if(NOT HEAVEN_CPU_ARCH_LOWER STREQUAL "portable")
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        message(FATAL_ERROR
            "HEAVEN_CPU_ARCH=${HEAVEN_CPU_ARCH_LOWER} requires an x86-64 target")
    endif()
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU" OR
       NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR
            "HEAVEN_CPU_ARCH=${HEAVEN_CPU_ARCH_LOWER} requires Clang or GCC")
    endif()

    if(HEAVEN_CPU_ARCH_LOWER STREQUAL "native")
        set(HEAVEN_CPU_MARCH native)
    else()
        string(REGEX REPLACE "^zen" "znver"
            HEAVEN_CPU_MARCH "${HEAVEN_CPU_ARCH_LOWER}")
    endif()

    include(CheckCCompilerFlag)
    include(CheckCXXCompilerFlag)
    # These checks depend on the selected architecture. Do not reuse a cached
    # success after switching an existing build tree from (for example) zen4 to
    # zen5.
    unset(HEAVEN_C_SUPPORTS_CPU_MARCH CACHE)
    unset(HEAVEN_CXX_SUPPORTS_CPU_MARCH CACHE)
    check_c_compiler_flag("-march=${HEAVEN_CPU_MARCH}" HEAVEN_C_SUPPORTS_CPU_MARCH)
    check_cxx_compiler_flag("-march=${HEAVEN_CPU_MARCH}" HEAVEN_CXX_SUPPORTS_CPU_MARCH)
    if(NOT HEAVEN_C_SUPPORTS_CPU_MARCH OR NOT HEAVEN_CXX_SUPPORTS_CPU_MARCH)
        message(FATAL_ERROR
            "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} does not support "
            "-march=${HEAVEN_CPU_MARCH} for both C and C++")
    endif()

    message(STATUS
        "Heaven CPU architecture: ${HEAVEN_CPU_ARCH_LOWER} (-march=${HEAVEN_CPU_MARCH})")
else()
    message(STATUS "Heaven CPU architecture: portable")
endif()

function(heaven_apply_cpu_arch target)
    if(DEFINED HEAVEN_CPU_MARCH)
        if(NOT TARGET "${target}")
            message(FATAL_ERROR "Unknown CPU-optimized target: ${target}")
        endif()
        target_compile_options("${target}" PRIVATE
            $<$<COMPILE_LANGUAGE:C>:-march=${HEAVEN_CPU_MARCH}>
            $<$<COMPILE_LANGUAGE:CXX>:-march=${HEAVEN_CPU_MARCH}>
        )
    endif()
endfunction()

# Filament's BlueVK and SDL load Vulkan dynamically. Link only the headers:
# an unconditional loader dependency prevents even OpenGL startup on machines
# without Vulkan installed. Android supplies these headers through the NDK.
if(NOT HEAVEN_PLATFORM_ANDROID)
    find_package(Vulkan REQUIRED)
    set(HEAVEN_VULKAN_LINK Vulkan::Headers)
endif()
# SDL2 for window + input. Use the native macOS SDL2 package for current Cocoa
# and Apple Silicon support; Linux retains its distro pkg-config dependency.
# Windows and Android build the full matching vendored SDL2 project.
if(HEAVEN_PLATFORM_MACOS)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    set(HEAVEN_SDL_INCLUDE_DIRS ${SDL2_INCLUDE_DIRS})
    foreach(include_dir IN LISTS SDL2_INCLUDE_DIRS)
        if(EXISTS "${include_dir}/SDL.h")
            get_filename_component(include_parent "${include_dir}" DIRECTORY)
            list(APPEND HEAVEN_SDL_INCLUDE_DIRS "${include_parent}")
        endif()
    endforeach()
    set(HEAVEN_SDL_LINK_LIBS PkgConfig::SDL2)
elseif(HEAVEN_PLATFORM_LINUX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    set(HEAVEN_SDL_INCLUDE_DIRS ${SDL2_INCLUDE_DIRS})
    set(HEAVEN_SDL_LINK_LIBS ${SDL2_LIBRARIES})
else()
    set(HEAVEN_SDL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/third_party/libsdl2"
        CACHE PATH "SDL2 source tree (native library and Android Java glue must match)")
    if(HEAVEN_PLATFORM_ANDROID)
        set(LIBTYPE SHARED CACHE STRING "" FORCE)
        set(SDL_SHARED ON CACHE BOOL "" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "" FORCE)
        set(HEAVEN_SDL_LINK_LIBS SDL2)
    else()
        set(LIBTYPE STATIC CACHE STRING "" FORCE)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(HEAVEN_SDL_LINK_LIBS SDL2-static)
    endif()
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(${HEAVEN_SDL_SOURCE_DIR}
                     ${CMAKE_CURRENT_BINARY_DIR}/sdl2)
    # Owned sources consistently include SDL2/SDL.h. Upstream SDL exports its
    # headers without the SDL2 parent directory; mirror them in the build tree.
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sdl-include/SDL2")
    file(GLOB HEAVEN_SDL_HEADERS "${HEAVEN_SDL_SOURCE_DIR}/include/*.h")
    foreach(header IN LISTS HEAVEN_SDL_HEADERS)
        get_filename_component(header_name "${header}" NAME)
        configure_file("${header}" "${CMAKE_CURRENT_BINARY_DIR}/sdl-include/SDL2/${header_name}" COPYONLY)
    endforeach()
    set(HEAVEN_SDL_INCLUDE_DIRS
        ${CMAKE_CURRENT_BINARY_DIR}/sdl-include
        ${HEAVEN_SDL_SOURCE_DIR}/include)
endif()

# GLM (header-only). For Android cross-builds this is still a host-side header
# dependency, so allow the normal host include paths or an explicit cache/env
# override instead of only searching the target sysroot.
find_path(GLM_INCLUDE_DIR
    NAMES glm/glm.hpp
    PATHS /usr/include /usr/local/include ENV GLM_INCLUDE_DIR
    NO_CMAKE_FIND_ROOT_PATH)
if(NOT GLM_INCLUDE_DIR)
    message(FATAL_ERROR "GLM headers not found; install glm or pass -DGLM_INCLUDE_DIR=/path/to/include")
endif()
if(HEAVEN_PLATFORM_ANDROID)
    # CMake filters host /usr/include out of NDK compile commands. Stage only
    # this header-only dependency; adding the host's full include tree would
    # mix incompatible libc and Android headers.
    file(COPY "${GLM_INCLUDE_DIR}/glm" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/glm-include")
    set(GLM_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/glm-include")
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HeavenCpuDependencies.cmake)

# ── Filament Rendering Engine ────────────────────────────────────
# Vendored as a subtree under lib/filament. macOS uses Vulkan via MoltenVK so
# the committed Vulkan material packets stay unchanged. Other platforms retain
# Vulkan with their existing OpenGL fallback.
set(FILAMENT_SKIP_SAMPLES ON CACHE BOOL "" FORCE)
set(FILAMENT_SKIP_SDL2 ON CACHE BOOL "" FORCE)
set(FILAMENT_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(FILAMENT_SUPPORTS_VULKAN ON CACHE BOOL "" FORCE)
set(FILAMENT_SUPPORTS_METAL OFF CACHE BOOL "" FORCE)
if(HEAVEN_PLATFORM_MACOS)
    set(FILAMENT_SUPPORTS_OPENGL OFF CACHE BOOL "" FORCE)
else()
    set(FILAMENT_SUPPORTS_OPENGL ON CACHE BOOL "" FORCE)
endif()
if(HEAVEN_PLATFORM_ANDROID)
    # Filament's Android EGL subclass requires its common EGL implementation.
    # The plain NDK toolchain does not set Filament's EGL platform switch.
    set(EGL ON)
endif()
if(HEAVEN_PLATFORM_LINUX)
    # Linux WSI: build BOTH X11 and Wayland surface paths into one
    # binary. Our patched lib/filament/CMakeLists.txt detects this
    # combination (Wayland + XCB/XLIB) and defines FILAMENT_LINUX_DUAL_WSI,
    # which makes VulkanPlatformLinux.cpp dispatch between Wayland and
    # X11 at runtime based on the tagged native-window handle we pass in.
    set(FILAMENT_SUPPORTS_WAYLAND ON CACHE BOOL "" FORCE)
    set(FILAMENT_SUPPORTS_XCB ON CACHE BOOL "" FORCE)
    set(FILAMENT_SUPPORTS_XLIB ON CACHE BOOL "" FORCE)
else()
    set(FILAMENT_SUPPORTS_WAYLAND OFF CACHE BOOL "" FORCE)
    set(FILAMENT_SUPPORTS_XCB OFF CACHE BOOL "" FORCE)
    set(FILAMENT_SUPPORTS_XLIB OFF CACHE BOOL "" FORCE)
endif()
set(FILAMENT_ENABLE_LTO OFF CACHE BOOL "" FORCE)
set(FILAMENT_ENABLE_RTTI ON CACHE BOOL "" FORCE)
set(FILAMENT_ENABLE_EXCEPTIONS ON CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament ${CMAKE_CURRENT_BINARY_DIR}/filament)

# Optimize the runtime dependency closure, not Filament's host-side generators.
# In particular, matc, resgen, and glslminifier are executed while building and
# must remain runnable on a generic x86-64 CI host. Libraries shared with those
# tools (utils, math, filabridge, smol-v, and zstd) likewise remain portable.
set(HEAVEN_CPU_OPTIMIZED_RUNTIME_TARGETS
    Jolt
    filament
    backend
    bluegl
    bluevk
    filaflat
    filagui
    geometry
    filament-iblprefilter
    imgui
    meshoptimizer
    mikktspace
)
foreach(runtime_target IN LISTS HEAVEN_CPU_OPTIMIZED_RUNTIME_TARGETS)
    if(TARGET "${runtime_target}")
        heaven_apply_cpu_arch("${runtime_target}")
    endif()
endforeach()

# ── Source files ─────────────────────────────────────────────────
#
# Engine = pure CPU code (mesh I/O, scene loading, animation,
# CPU tessellation, cloth/rope sim, material DB). No GPU API
# dependencies — these compile against the RHI interface.

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HeavenCpuEngine.cmake)

# The audio device and interactive camera remain application concerns.
set(ENGINE_SOURCES
    src/engine/camera.cpp
    src/engine/loading_benchmark.cpp
    src/engine/music_player.cpp
    third_party/stb_vorbis.c
)

# The bundled decoder is a third-party C translation unit. Keep owned-target
# C++ diagnostics from leaking into it, and suppress compiler-version-specific
# warnings without modifying the vendored source.
if(MSVC)
    set_source_files_properties(third_party/stb_vorbis.c
        PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties(third_party/stb_vorbis.c
        PROPERTIES COMPILE_OPTIONS "-w")
endif()

# RHI = the Filament backend implementation plus the abstract
# IRhiBackend interface. The hand-rolled GL 4.5 backend that used
# to live alongside it was retired in v0.4.
set(RHI_SOURCES
    src/rhi/rhi.cpp
    src/rhi/render_test_scene_assets.cpp
    src/rhi/backend_factory.cpp
    src/rhi/filament_backend.cpp
    src/rhi/frame_generation/frame_generation_runtime.cpp
    src/rhi/frame_generation/frame_generation_output.cpp
    src/rhi/frame_generation/temporal_scene.cpp
    src/rhi/frame_generation/temporal_effects.cpp
    src/rhi/frame_generation/temporal_grass.cpp
    src/rhi/filament_util.cpp
    src/rhi/post_process.cpp
    src/rhi/depth_prepass.cpp
    src/rhi/scene_color_capture.cpp
    src/rhi/hdr_output.cpp
    src/rhi/png_image.cpp
    src/rhi/loading_screen.cpp
    src/rhi/texture_store.cpp
    src/rhi/height_sample_batch.cpp
    src/rhi/packed_static_mesh.cpp
    src/rhi/displacement_surface_preparer.cpp
    src/rhi/prepared_displacement_cache.cpp
    src/rhi/displacement_carrier.cpp
    src/rhi/scene_displacement_preparation.cpp
    src/rhi/mesh_store.cpp
    src/rhi/material_tuning_bindings.cpp
    src/rhi/material_instance_cache.cpp
    src/rhi/scene_builder.cpp
    src/rhi/depth_proxy_builder.cpp
    src/rhi/grass_depth_proxy.cpp
    src/rhi/scene_light_spawner.cpp
    src/rhi/lighting_system.cpp
    src/rhi/sky_atmosphere.cpp
    src/rhi/sky_ibl.cpp
    src/rhi/features/cloud_feature.cpp
    src/rhi/features/volumetric_cloud_feature.cpp
    src/rhi/features/godrays_feature.cpp
    src/rhi/features/ssgi_feature.cpp
    src/rhi/features/volfog_feature.cpp
    src/rhi/features/grass_feature.cpp
    src/rhi/features/massive_foliage_feature.cpp
    src/rhi/features/sky_feature.cpp
    src/rhi/features/particle_feature.cpp
    src/rhi/features/lens_flare_feature.cpp
    src/rhi/features/wind_arrows_feature.cpp
    src/rhi/features/rain_feature.cpp
    src/rhi/features/wet_reflections_feature.cpp
    src/rhi/features/advanced_materials_feature.cpp
    src/rhi/overlay/overlay_ui.cpp
    src/rhi/overlay/microprofiler_ui.cpp
    src/rhi/profile_bridge.cpp
    # GPU-free Settings Catalog (single source of truth for editable settings).
    # Depends only on engine headers + config_edit.h, so it also links into the
    # headless heaven_benchmark_tests host target below.
    src/rhi/overlay/settings_catalog.cpp
    # GPU-free configuration controller (maps edited settings back to AppConfig
    # + config_file persistence). Depends only on config_edit.h,
    # settings_catalog.h, engine/config_file.h, engine/types.h, so it also links
    # Filament-free into the headless heaven_benchmark_tests host target below.
    src/rhi/overlay/config_controller.cpp
)

if(HEAVEN_PLATFORM_MACOS)
    list(APPEND RHI_SOURCES src/rhi/macos_surface.mm)
endif()

option(HEAVEN_ENABLE_FSR3 "Experimental FSR 3.1 Vulkan frame generation" OFF)
if(HEAVEN_ENABLE_FSR3)
    if(NOT HEAVEN_PLATFORM_LINUX AND NOT HEAVEN_PLATFORM_WINDOWS)
        message(FATAL_ERROR "FSR presentation is currently enabled only for Linux and Windows")
    endif()
    add_subdirectory(src/rhi/frame_generation)
    list(APPEND RHI_SOURCES src/rhi/frame_generation/frame_generation_platform.cpp
        src/rhi/frame_generation/frame_generation_capture.cpp
        src/rhi/frame_generation/frame_generation_present_trace.cpp)
    set(fg_ui_spirv "${CMAKE_CURRENT_BINARY_DIR}/fg_ui.spv")
    set(fg_ui_header "${CMAKE_CURRENT_BINARY_DIR}/fg_ui_spirv.h")
    add_custom_command(OUTPUT "${fg_ui_header}"
        COMMAND "${HEAVEN_FSR_GLSLANG}" -V --target-env vulkan1.1
            "${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/frame_generation/fg_ui.comp" -o "${fg_ui_spirv}"
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/embed_spirv.py"
            "${fg_ui_spirv}" "${fg_ui_header}"
        DEPENDS src/rhi/frame_generation/fg_ui.comp ../scripts/embed_spirv.py VERBATIM)
    list(APPEND RHI_SOURCES "${fg_ui_header}")
endif()

# Benchmark = pure CPU logic (frame stats, scoring, scene markers, the
# run state machine). GLOB so new logic files (stats_collector,
# score_calculator, scene_markers, benchmark_system) added by later
# spec tasks are picked up automatically without editing this list.
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HeavenDlss.cmake)

file(GLOB BENCHMARK_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/benchmark/*.cpp
)
list(REMOVE_ITEM BENCHMARK_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/benchmark/scene_markers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/benchmark/microprofiler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/benchmark/microprofile_report.cpp)

if(HEAVEN_PLATFORM_ANDROID)
    add_library(heaven_benchmark SHARED src/main.cpp
        ../android/native/android_main.cpp ${ENGINE_SOURCES} ${RHI_SOURCES} ${BENCHMARK_SOURCES})
    set_source_files_properties(src/main.cpp PROPERTIES COMPILE_DEFINITIONS "main=heaven_main")
else()
    add_executable(heaven_benchmark src/main.cpp
        ${ENGINE_SOURCES} ${RHI_SOURCES} ${BENCHMARK_SOURCES})
endif()

# Source directory for renderer-owned runtime assets, including the sky and
# startup loading screen. This is consumed only by the --pack-assets startup
# mode; normal rendering resolves installed copies under configured Data_Root.
set(HEAVEN_BUNDLED_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    CACHE STRING "Fallback directory for renderer-owned asset packer inputs")
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tools/asset_packer
                 ${CMAKE_CURRENT_BINARY_DIR}/asset_packer)


# ── Material compilation ─────────────────────────────────────────
# Compile .mat files to .filamat using matc, then embed as C++ data.
# The generated header and source are checked in;
# regenerate with `./scripts/build.sh --materials`.
# Compile the byte arrays once; consumers parse only their small declarations.
add_library(heaven_materials_data STATIC src/rhi/materials_data.cpp)
target_compile_features(heaven_materials_data PRIVATE cxx_std_17)
set_target_properties(heaven_materials_data PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_CLANG_TIDY ""
    POSITION_INDEPENDENT_CODE ON
)

set(MATC_TOOL ${CMAKE_CURRENT_BINARY_DIR}/filament/tools/matc/matc)
set(MAT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/materials)
set(MAT_OUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/materials)

add_custom_target(compile_materials
    COMMAND ${MATC_TOOL} -a all -p desktop -o ${MAT_OUT_DIR}/heaven_lit.filamat ${MAT_DIR}/heaven_lit.mat
    COMMAND ${MATC_TOOL} -a all -p desktop -o ${MAT_OUT_DIR}/heaven_lit_twosided.filamat ${MAT_DIR}/heaven_lit_twosided.mat
    COMMAND ${MATC_TOOL} -a all -p desktop -o ${MAT_OUT_DIR}/heaven_unlit.filamat ${MAT_DIR}/heaven_unlit.mat
    COMMAND ${MATC_TOOL} -a all -p desktop -o ${MAT_OUT_DIR}/heaven_grass.filamat ${MAT_DIR}/heaven_grass.mat
    COMMAND ${MATC_TOOL} -a all -p desktop -o ${MAT_OUT_DIR}/heaven_godrays.filamat ${MAT_DIR}/heaven_godrays.mat
    COMMAND ${MATC_TOOL} -a all -p desktop -o ${MAT_OUT_DIR}/heaven_clouds.filamat ${MAT_DIR}/heaven_clouds.mat
    COMMAND ${MATC_TOOL} -a all -p desktop -o ${MAT_OUT_DIR}/heaven_blit.filamat ${MAT_DIR}/heaven_blit.mat
    COMMAND ${MATC_TOOL} -a all -p desktop -o ${MAT_OUT_DIR}/heaven_heat_haze.filamat ${MAT_DIR}/heaven_heat_haze.mat
    COMMAND ${MATC_TOOL} -a all -p desktop -o ${MAT_OUT_DIR}/heaven_glass.filamat ${MAT_DIR}/heaven_glass.mat
    COMMAND ${MATC_TOOL} -a all -p desktop -o ${MAT_OUT_DIR}/heaven_volumetric_clouds.filamat ${MAT_DIR}/heaven_volumetric_clouds.mat
    COMMAND ${MATC_TOOL} -a all -p desktop -o ${MAT_OUT_DIR}/heaven_ssgi.filamat ${MAT_DIR}/heaven_ssgi.mat
    DEPENDS matc
    COMMENT "Compiling Filament materials with matc"
    VERBATIM
)

target_include_directories(heaven_benchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_include_directories(heaven_benchmark SYSTEM PRIVATE
    ${HEAVEN_SDL_INCLUDE_DIRS}
    ${GLM_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/JoltPhysics
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/pocketfft
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/bcdec
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/third_party/stb
    # Vendored stb_vorbis single-file Ogg Vorbis decoder (public domain, no
    # link/package). Included by the music subsystem via third_party/stb_vorbis.h.
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    # Vendored nlohmann/json single header — included only by config_file.cpp
    # (MIT, dependency-free; no Filament/SDL/GPU transitive dependency).
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/nlohmann
    # Filament public headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/filament/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/filament/backend/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/utils/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/math/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/geometry/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/filabridge/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/filaflat/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/filameshio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/image/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/camutils/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/iblprefilter/include
    # Overlay UI — vendored Dear ImGui + filagui (no new external dep). The
    # filagui/imgui targets export these PUBLIC, but list them explicitly so
    # the backend TU finds <filagui/ImGuiHelper.h> and <imgui.h>.
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/filagui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/third_party/imgui
    # BlueVK (Filament's vendored Vulkan loader). backend PUBLIC-links it,
    # but list it explicitly so the backend TU finds <bluevk/BlueVK.h> for
    # the GPU-info query (Req 12.4). No new external dependency.
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/bluevk/include
)

target_link_libraries(heaven_benchmark PRIVATE
    heaven_asset_packer_lib
    heaven_materials_data
    ${HEAVEN_VULKAN_LINK}
    ${HEAVEN_SDL_LINK_LIBS}
    Jolt
    libzip::zip
    # Filament libraries
    filament
    backend
    utils
    math
    geometry
    filabridge
    filaflat
    filament-iblprefilter
    stb
    # Overlay UI — vendored filagui (Filament-native ImGui renderer) +
    # Dear ImGui. filagui PUBLIC-links imgui, but list both for clarity.
    filagui
    imgui
)

heaven_configure_owned_cpp_target(heaven_benchmark)
heaven_apply_cpu_arch(heaven_benchmark)
heaven_link_dlss(heaven_benchmark)
target_compile_definitions(heaven_benchmark PRIVATE
    HEAVEN_HAS_FSR3=$<BOOL:${HEAVEN_ENABLE_FSR3}>)
if(NOT MSVC)
    target_compile_options(heaven_benchmark PRIVATE
        -Wno-unused-parameter
        -Wno-unused-function
        -Wno-missing-field-initializers
    )
endif()
if(NOT HEAVEN_PLATFORM_LINUX)
    target_compile_definitions(heaven_benchmark PRIVATE SDL_MAIN_HANDLED)
endif()
target_compile_definitions(heaven_benchmark PRIVATE
    HEAVEN_BUNDLED_ASSET_DIR="${HEAVEN_BUNDLED_ASSET_DIR}"
    HEAVENBENCH_VERSION="${PROJECT_VERSION}"
    HEAVENBENCH_ARCHITECTURE="${HEAVEN_CPU_ARCH_LOWER}"
)

# Persistent geometry is invalidated by real producer/dependency source edits,
# as well as the complete frozen source and policy keyed at runtime. Filament
# types appear only in the CPU header contract; this module never uses a GPU.
file(GLOB_RECURSE PREPARED_CACHE_DEPENDENCY_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/OpenSubdiv/opensubdiv/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/OpenSubdiv/opensubdiv/*.cpp)
set(PREPARED_CACHE_PRODUCER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/bounded_displacement_surface.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/bounded_displacement_surface.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/displacement_surface_preparer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/displacement_surface_preparer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/prepared_displacement_cache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/prepared_displacement_cache.h
    ${PREPARED_CACHE_DEPENDENCY_SOURCES})
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PREPARED_CACHE_PRODUCER_SOURCES})
set(PREPARED_CACHE_SOURCE_DIGESTS "")
foreach(CACHE_SOURCE IN LISTS PREPARED_CACHE_PRODUCER_SOURCES)
    file(SHA256 ${CACHE_SOURCE} CACHE_SOURCE_SHA)
    string(APPEND PREPARED_CACHE_SOURCE_DIGESTS "${CACHE_SOURCE_SHA};")
endforeach()
string(SHA256 PREPARED_CACHE_BUILD_ID "${PREPARED_CACHE_SOURCE_DIGESTS}")
set_source_files_properties(src/rhi/prepared_displacement_cache.cpp PROPERTIES
    COMPILE_DEFINITIONS "HEAVEN_PREPARED_CACHE_BUILD_ID=\"${PREPARED_CACHE_BUILD_ID}\"")

# ── Renderer-linked smoke harness (heaven_benchmark_smoke) ───────
if(NOT HEAVEN_PLATFORM_ANDROID)
#
# A GATED smoke-test executable that links the FULL renderer (the same
# Filament/SDL/Jolt libraries as heaven_benchmark) so it can exercise the live
# Filament backend. Unlike heaven_benchmark_tests (headless, Filament-free,
# RapidCheck), this target needs a GPU/display, so its driver self-gates:
# without HEAVEN_SMOKE=1 or a display it prints SKIPPED and exits 0, keeping
# headless CI green (spec testing strategy: Filament smoke tests gated on
# hardware/display).
#
# It is assembled from the SAME source lists as heaven_benchmark MINUS
# src/main.cpp, plus the smoke driver(s) under smoke/ (so there is exactly one
# main()). GLOB the smoke drivers so task 10.5 can drop in another case file.
file(GLOB BENCHMARK_SMOKE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/smoke/*.cpp
)

add_executable(heaven_benchmark_smoke
    ${BENCHMARK_SMOKE_SOURCES}
    tests/smoke_specular_occlusion.cpp
    ${ENGINE_SOURCES}
    ${RHI_SOURCES}
    ${BENCHMARK_SOURCES}
)
# Native swapchain fault injection is compiled only into this GPU test binary.
# The ordinary benchmark and SDK-off paths contain no injection branch.
target_compile_definitions(heaven_benchmark_smoke PRIVATE HEAVEN_FG_FAULT_INJECTION=1)

target_include_directories(heaven_benchmark_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_include_directories(heaven_benchmark_smoke SYSTEM PRIVATE
    ${HEAVEN_SDL_INCLUDE_DIRS}
    ${GLM_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/JoltPhysics
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/pocketfft
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/bcdec
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/third_party/stb
    # Vendored stb_vorbis single-file Ogg Vorbis decoder — third_party/stb_vorbis.c
    # is part of ENGINE_SOURCES, which this target also compiles, so it needs the
    # include (same as heaven_benchmark).
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    # Vendored nlohmann/json single header — config_file.cpp is part of
    # ENGINE_SOURCES, which this target also compiles, so it needs the include.
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/nlohmann
    # Filament public headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/filament/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/filament/backend/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/utils/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/math/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/geometry/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/filabridge/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/filaflat/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/filameshio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/image/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/camutils/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/iblprefilter/include
    # Overlay UI — vendored Dear ImGui + filagui (same as heaven_benchmark).
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/filagui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/third_party/imgui
    # BlueVK (Filament's vendored Vulkan loader) — same as heaven_benchmark.
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/bluevk/include
)

target_link_libraries(heaven_benchmark_smoke PRIVATE
    heaven_asset_packer_lib
    heaven_materials_data
    ${HEAVEN_VULKAN_LINK}
    ${HEAVEN_SDL_LINK_LIBS}
    Jolt
    libzip::zip
    # Filament libraries
    filament
    backend
    utils
    math
    geometry
    filabridge
    filaflat
    filament-iblprefilter
    stb
    # Overlay UI — vendored filagui + Dear ImGui.
    filagui
    imgui
)

heaven_configure_owned_cpp_target(heaven_benchmark_smoke)
heaven_apply_cpu_arch(heaven_benchmark_smoke)
heaven_link_dlss(heaven_benchmark_smoke)
if(NOT MSVC)
    target_compile_options(heaven_benchmark_smoke PRIVATE
        -Wno-unused-parameter
        -Wno-unused-function
        -Wno-missing-field-initializers
    )
endif()
if(NOT HEAVEN_PLATFORM_LINUX)
    target_compile_definitions(heaven_benchmark_smoke PRIVATE SDL_MAIN_HANDLED)
endif()
target_compile_definitions(heaven_benchmark_smoke PRIVATE
    HEAVEN_BUNDLED_ASSET_DIR="${HEAVEN_BUNDLED_ASSET_DIR}"
)

foreach(render_target IN ITEMS heaven_benchmark heaven_benchmark_smoke)
    if(HEAVEN_ENABLE_FSR3)
        target_include_directories(${render_target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_link_libraries(${render_target} PRIVATE heaven_fsr31_provider)
    endif()
endforeach()
target_compile_definitions(heaven_benchmark_smoke PRIVATE
    HEAVEN_HAS_FSR3=$<BOOL:${HEAVEN_ENABLE_FSR3}>)

if(HEAVEN_PLATFORM_LINUX)
    # BlueGL's generated assembly contains fixed function-pointer trampolines,
    # but omits the GNU-stack note. It needs no executable stack; make that
    # explicit on our executables without changing the vendored assembly.
    foreach(render_target IN ITEMS heaven_benchmark heaven_benchmark_smoke)
        target_link_options(${render_target} PRIVATE "LINKER:-z,noexecstack")
    endforeach()
elseif(HEAVEN_PLATFORM_MACOS)
    foreach(render_target IN ITEMS heaven_benchmark heaven_benchmark_smoke)
        target_link_libraries(${render_target} PRIVATE "-framework Cocoa" "-framework QuartzCore")
    endforeach()
endif()

# ── Host-runnable test harness (heaven_benchmark_tests) ──────────


#
# RapidCheck is a TEST-ONLY vendored dependency under lib/rapidcheck.
# It is added here, guarded, and linked ONLY into the test executable —
# never into heaven_benchmark (AGENTS.md §5: no new runtime deps).
#
# The test target compiles the PURE-LOGIC translation units only
# (src/benchmark/*.cpp plus the camera/camera_path engine TUs) without
# Filament, SDL, or ImGui, so the property/unit tests run on the host
# without a GPU. camera.cpp uses SDL scancode constants and
# unit_rhi_driver_hint.cpp uses SDL hint calls, so the same platform-selected
# SDL dependency is shared with this test target; asset loaders do not need it.

# Keep RapidCheck's own tests/examples/integrations off — we only want
# the `rapidcheck` static library target.
set(RC_ENABLE_TESTS    OFF CACHE BOOL "" FORCE)
set(RC_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/rapidcheck
                 ${CMAKE_CURRENT_BINARY_DIR}/rapidcheck)

# Dependency targets can publish their own include directories. Mark those
# targets as system-owned as well as the explicit include paths above so owned
# diagnostics never leak into vendored RapidCheck, ImGui, or Jolt headers.
set_target_properties(rapidcheck imgui Jolt PROPERTIES SYSTEM TRUE)

# Pure-logic sources under test. GLOB so logic files added by later spec
# tasks (stats_collector, score_calculator, scene_markers, benchmark_system)
# are picked up automatically.
set(BENCHMARK_TEST_LOGIC_SOURCES ${BENCHMARK_SOURCES})

# Test translation units. GLOB so new benchmark/tests/*.cpp files are
# compiled in automatically.
file(GLOB BENCHMARK_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp
)
list(FILTER BENCHMARK_TEST_SOURCES EXCLUDE REGEX "/smoke_specular_occlusion\\.cpp$")

add_executable(heaven_benchmark_tests
    ${BENCHMARK_TEST_LOGIC_SOURCES}
    src/engine/camera.cpp
    src/rhi/prepared_displacement_cache.cpp
    src/rhi/overlay/overlay_ui.cpp
    src/rhi/overlay/microprofiler_ui.cpp
    # GPU-free Settings Catalog — links Filament-free into the headless host
    # test target (depends only on engine headers + config_edit.h).
    src/rhi/overlay/settings_catalog.cpp
    # GPU-free configuration controller — links Filament-free into the headless
    # host test target (depends only on config_edit.h, settings_catalog.h,
    # engine/config_file.h, engine/types.h).
    src/rhi/overlay/config_controller.cpp
    ${BENCHMARK_TEST_SOURCES}
)
# The documented host-test target also prepares its registered CLI fixture.
add_dependencies(heaven_benchmark_tests heaven_asset_packer)

target_include_directories(heaven_benchmark_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_include_directories(heaven_benchmark_tests SYSTEM PRIVATE
    ${GLM_INCLUDE_DIR}
    ${HEAVEN_SDL_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/rapidcheck/include
    # Jolt headers — the engine sim TUs (rope/cloth/collision) reached through
    # the packer's discovery driver include Jolt; the static `Jolt` lib is on
    # the link line. Jolt is CPU-only, so the test target stays GPU/display-free.
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/JoltPhysics
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/pocketfft
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/bcdec
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/third_party/stb
    # Vendored nlohmann/json single header — included only by config_file.cpp.
    # Header-only, dependency-free; keeps the test link line Filament/SDL-free.
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/nlohmann
    # Dear ImGui headers for the headless OverlayUI widget builder. The vendored
    # `imgui` target is a standalone STATIC library (lib/filament/third_party/
    # imgui/tnt) with NO Filament dependency, so the overlay code compiles and
    # links headless without dragging in Filament/SDL.
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/third_party/imgui
    # Filament public + math headers (header-only for the helpers exercised by
    # tests). prop_filament_bounds.cpp validates the real filament::Box
    # min/max conversion without linking the renderer or requiring a GPU.
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/filament/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/math/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/utils/include
    # Header-only HDR surface-format validation; no Vulkan loader link.
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/bluevk/include
)

# RapidCheck (test framework) + GLM (header-only) + the self-contained Dear
# ImGui static library. ImGui (`imgui`) carries no Filament/SDL dependency, so
# OverlayUI is exercised headless; NEITHER filagui NOR filament is linked here.
# Jolt (CPU physics) backs the engine sim TUs the packer's discovery reuses.
target_link_libraries(heaven_benchmark_tests PRIVATE
    heaven_asset_packer_lib
    rapidcheck
    imgui
    Jolt
    libzip::zip
    # SDL2 (host-link only): tests/unit_rhi_driver_hint.cpp exercises the
    # SDL_HINT_VIDEODRIVER hint that rhi.cpp uses under --hdr. SDL_SetHint /
    # SDL_GetHint are library calls (not header-only) and do NOT require
    # SDL_Init or a GPU/display, so the harness stays host-runnable.
    ${HEAVEN_SDL_LINK_LIBS}
)

heaven_configure_owned_cpp_target(heaven_benchmark_tests)
heaven_apply_cpu_arch(heaven_benchmark_tests)
if(NOT MSVC)
    target_compile_options(heaven_benchmark_tests PRIVATE
        -Wno-unused-parameter
        -Wno-unused-function
        -Wno-missing-field-initializers
    )
endif()
if(NOT HEAVEN_PLATFORM_LINUX)
    target_compile_definitions(heaven_benchmark_tests PRIVATE SDL_MAIN_HANDLED)
endif()

# Absolute path to the rendering-feature checklist, baked in for the
# docs-consistency test (tests/unit_docs_consistency.cpp, task 11.3). ctest runs
# from the build dir, so a relative path would be fragile; this resolves the
# repo-root doc robustly. Scoped to this target only.
target_compile_definitions(heaven_benchmark_tests PRIVATE
    HEAVEN_BUNDLED_ASSET_DIR="${HEAVEN_BUNDLED_ASSET_DIR}"
    WEATHER_DOCS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../docs/RENDERING_FEATURES.md"
)

# Absolute paths to the two renderer source files asserted on by the cleanup
# structural test (tests/unit_cleanup_structural.cpp, task 21.2). It reads the
# post-21.1 source at runtime to confirm the shipping-path debug printfs and the
# dead MeshData accumulator were removed. ctest runs from the build dir, so a
# relative path would be fragile. Scoped to this target only.
target_compile_definitions(heaven_benchmark_tests PRIVATE
    SCENE_BUILDER_SRC_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/scene_builder.cpp"
    MESH_STORE_SRC_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/mesh_store.cpp"
)

# Absolute paths to the renderer sources asserted on by the Phase-8 structural
# single-registration test (tests/unit_phase8_structural.cpp, task 10.2). It
# reads the source at runtime to confirm AdvancedMaterialsFeature is registered
# exactly once, that FilamentBackend gains no per-effect Phase-8 data member,
# and that render_frame inlines no per-effect work (Req 13.5 / 16.3). ctest runs
# from the build dir, so a relative path would be fragile. Scoped to this target.
target_compile_definitions(heaven_benchmark_tests PRIVATE
    FILAMENT_BACKEND_SRC_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/filament_backend.cpp"
    FILAMENT_BACKEND_HDR_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/filament_backend.h"
    LIGHTING_SYSTEM_SRC_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/lighting_system.cpp"
    ADVANCED_MATERIALS_FEATURE_HDR_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/features/advanced_materials_feature.h"
    MATERIAL_TUNING_BINDINGS_SRC_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/material_tuning_bindings.cpp"
    MATERIAL_INSTANCE_CACHE_SRC_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/material_instance_cache.cpp"
    GRASS_DEPTH_PROXY_SRC_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/grass_depth_proxy.cpp"
    RENDER_FEATURES_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/features"
    FILAMENT_MATERIALS_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/materials"
)

# Register with CTest so `ctest` in the build dir runs the suite.
enable_testing()
add_executable(heaven_capture_camera_motion_tests
    tests/capture_camera_motion_contract_test.cpp)
heaven_configure_owned_cpp_target(heaven_capture_camera_motion_tests)
target_include_directories(heaven_capture_camera_motion_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(heaven_capture_camera_motion_tests PRIVATE HEAVEN_CAPTURE_CAMERA_MOTION_TEST_MAIN)
add_test(NAME heaven_capture_camera_motion_tests COMMAND heaven_capture_camera_motion_tests)
add_executable(heaven_frame_generation_history_tests
    src/rhi/frame_generation/frame_generation_history_test.cpp)
target_compile_features(heaven_frame_generation_history_tests PRIVATE cxx_std_17)
add_test(NAME heaven_frame_generation_history_tests COMMAND heaven_frame_generation_history_tests)
find_package(Threads REQUIRED)
add_executable(heaven_frame_generation_present_trace_tests
    src/rhi/frame_generation/frame_generation_present_trace_test.cpp
    src/rhi/frame_generation/frame_generation_present_trace.cpp)
heaven_configure_owned_cpp_target(heaven_frame_generation_present_trace_tests)
target_include_directories(heaven_frame_generation_present_trace_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/../lib/nlohmann")
target_link_libraries(heaven_frame_generation_present_trace_tests PRIVATE Threads::Threads)
add_test(NAME heaven_frame_generation_present_trace_tests
    COMMAND heaven_frame_generation_present_trace_tests
            "${CMAKE_CURRENT_BINARY_DIR}/present-trace-tests")
if(HEAVEN_ENABLE_FSR3)
    add_executable(heaven_frame_generation_capture_contract_tests
        src/rhi/frame_generation/frame_generation_capture_contract_test.cpp
        src/rhi/frame_generation/frame_generation_capture.cpp)
    heaven_configure_owned_cpp_target(heaven_frame_generation_capture_contract_tests)
    target_link_libraries(heaven_frame_generation_capture_contract_tests PRIVATE
        bluevk ${HEAVEN_VULKAN_LINK})
    add_test(NAME heaven_frame_generation_capture_contract_tests
        COMMAND heaven_frame_generation_capture_contract_tests
            "${CMAKE_CURRENT_BINARY_DIR}/capture-contract-tests")
endif()
add_executable(heaven_filament_ssr_variant_tests
    src/rhi/frame_generation/filament_ssr_variant_test.cpp
    ../lib/filament/libs/filabridge/src/Variant.cpp
    ../lib/filament/libs/filamat/src/MaterialVariants.cpp)
# This fixture compiles Filament sources and includes DriverEnums.h, whose
# designated stencil initializers require C++20 on native MSVC.
target_compile_features(heaven_filament_ssr_variant_tests PRIVATE cxx_std_20)
target_include_directories(heaven_filament_ssr_variant_tests SYSTEM PRIVATE
    ../lib/filament/libs/filabridge/include
    ../lib/filament/filament/include
    ../lib/filament/filament/backend/include
    ../lib/filament/libs/utils/include
    ../lib/filament/libs/math/include
    ../lib/filament/libs/filamat/include
    ../lib/filament/libs/filamat/src
    ../lib/filament/filament/src)
add_test(NAME heaven_filament_ssr_variant_tests COMMAND heaven_filament_ssr_variant_tests)
add_executable(heaven_vulkan_pipeline_lifetime_tests
    ../lib/filament/filament/backend/test/test_VulkanPipelineLifetime.cpp)
target_compile_features(heaven_vulkan_pipeline_lifetime_tests PRIVATE cxx_std_17)
target_include_directories(heaven_vulkan_pipeline_lifetime_tests PRIVATE
    ../lib/filament/filament/backend/src)
add_test(NAME heaven_vulkan_pipeline_lifetime_tests COMMAND heaven_vulkan_pipeline_lifetime_tests)
if(TARGET heaven_fsr31_pipeline_lifetime_test)
    add_test(NAME heaven_fsr31_pipeline_lifetime_test COMMAND heaven_fsr31_pipeline_lifetime_test)
endif()
if(TARGET heaven_fsr31_view_retirement_test)
    add_test(NAME heaven_fsr31_view_retirement_test COMMAND heaven_fsr31_view_retirement_test)
    foreach(view_negative IN ITEMS old-double-rotation unbounded-inflight)
        add_test(NAME heaven_fsr31_view_retirement_${view_negative}
            COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/frame_generation/check_fsr31_view_negative.py"
                --executable "$<TARGET_FILE:heaven_fsr31_view_retirement_test>" --case "${view_negative}")
    endforeach()
endif()
add_executable(heaven_dlss_contract_tests
    src/rhi/frame_generation/dlss_contract_tests.cpp)
heaven_configure_owned_cpp_target(heaven_dlss_contract_tests)
target_compile_definitions(heaven_dlss_contract_tests PRIVATE HEAVEN_DLSS_CONTRACT_TEST_MAIN)
target_link_libraries(heaven_dlss_contract_tests PRIVATE heaven_dlss_provider)
add_test(NAME heaven_dlss_contract_tests COMMAND heaven_dlss_contract_tests)
add_executable(heaven_dlss_runtime_bridge_tests
    src/rhi/frame_generation/dlss_runtime_bridge_tests.cpp
    src/engine/frame_generation.cpp)
heaven_configure_owned_cpp_target(heaven_dlss_runtime_bridge_tests)
target_compile_definitions(heaven_dlss_runtime_bridge_tests PRIVATE
    HEAVEN_DLSS_RUNTIME_INPUT_TEST_MAIN)
target_include_directories(heaven_dlss_runtime_bridge_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(heaven_dlss_runtime_bridge_tests SYSTEM PRIVATE
    "${GLM_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/filament/backend/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/bluevk/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/utils/include")
target_link_libraries(heaven_dlss_runtime_bridge_tests PRIVATE ${HEAVEN_VULKAN_LINK})
add_test(NAME heaven_dlss_runtime_bridge_tests COMMAND heaven_dlss_runtime_bridge_tests)
add_executable(heaven_dlss_frame_token_tests
    src/rhi/frame_generation/dlss_frame_lifecycle_tests.cpp)
heaven_configure_owned_cpp_target(heaven_dlss_frame_token_tests)
target_include_directories(heaven_dlss_frame_token_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
add_test(NAME heaven_dlss_frame_token_tests COMMAND heaven_dlss_frame_token_tests)
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HeavenTemporalSceneTest.cmake)
add_executable(heaven_profiler_trace_tests
    ../lib/filament/filament/backend/test/test_ProfilerTrace.cpp)
target_include_directories(heaven_profiler_trace_tests PRIVATE
    ../lib/filament/filament/backend/src)
target_link_libraries(heaven_profiler_trace_tests PRIVATE backend utils)
target_compile_features(heaven_profiler_trace_tests PRIVATE cxx_std_23)
add_test(NAME heaven_profiler_trace_tests COMMAND heaven_profiler_trace_tests)
add_test(NAME heaven_benchmark_tests COMMAND heaven_benchmark_tests)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME heaven_frame_generation_present_analysis
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../tests/frame_generation_present_analysis_test.py)
    add_test(NAME heaven_fsr31_fetch_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fetch_fsr31_test.py)
    add_test(NAME heaven_dlss_lifecycle_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tests/dlss_lifecycle_contract_test.py)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
        # CPU-only controller checks; the real GPU pause fixture stays opt-in.
        add_test(NAME heaven_frame_generation_pause_controller
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/frame_generation/frame_generation_process_pause_test.py
                    --self-test --work-dir "${CMAKE_CURRENT_BINARY_DIR}/frame-generation-pause-controller")
        set_tests_properties(heaven_frame_generation_pause_controller PROPERTIES TIMEOUT 30)
        add_test(NAME heaven_frame_generation_suspend_controller
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/frame_generation/frame_generation_system_suspend_test.py
                    --self-test --work-dir "${CMAKE_CURRENT_BINARY_DIR}/frame-generation-suspend-controller")
        set_tests_properties(heaven_frame_generation_suspend_controller PROPERTIES TIMEOUT 30)
    endif()
    add_test(NAME heaven_fsr31_source_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/frame_generation/verify_fsr31_source_test.py)
    add_test(NAME heaven_microprofile_analysis
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/microprofile_analyzer_test.py)
    add_test(NAME heaven_runtime_paths_cli
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_paths_cli_test.py
                $<TARGET_FILE:heaven_benchmark>)
    add_test(NAME heaven_asset_packer_cli
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/asset_packer/test_cli.py
                $<TARGET_FILE:heaven_asset_packer>)
    add_test(NAME heaven_gtao_bent_normal_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/gtao_bent_normal_contract_test.py
                --filament ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament
                --matc $<TARGET_FILE:matc>)
    add_test(NAME heaven_specular_occluded_moment_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/specular_occluded_moment_contract_test.py
                --filament ${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament
                --matc $<TARGET_FILE:matc>)
    # Execute the actual material helper with a host compiler. These desktop
    # tests are not registered in Android builds; cross builds need a host tool.
    if(CMAKE_CROSSCOMPILING)
        find_program(HEAVEN_SURFACE_COLOR_HOST_CXX NAMES c++ clang++ g++ cl
            NO_CMAKE_FIND_ROOT_PATH)
    else()
        set(HEAVEN_SURFACE_COLOR_HOST_CXX "${CMAKE_CXX_COMPILER}")
    endif()
    if(HEAVEN_SURFACE_COLOR_HOST_CXX)
        add_test(NAME heaven_frame_generation_capture_memory_contract
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/frame_generation/frame_generation_capture_memory_contract_test.py
                    --cxx "${HEAVEN_SURFACE_COLOR_HOST_CXX}"
                    --vulkan-include "${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/bluevk/include"
                    --work-dir "${CMAKE_CURRENT_BINARY_DIR}/capture-memory-contract")
        set_tests_properties(heaven_frame_generation_capture_memory_contract PROPERTIES TIMEOUT 120)
        add_test(NAME heaven_filament_linear_staging_contract
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/filament_linear_staging_test.py
                    --cxx "${HEAVEN_SURFACE_COLOR_HOST_CXX}"
                    --vulkan-include "${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/bluevk/include"
                    --work-dir "${CMAKE_CURRENT_BINARY_DIR}/linear-staging-contract")
        set_tests_properties(heaven_filament_linear_staging_contract PROPERTIES TIMEOUT 120)
        add_test(NAME heaven_frame_generation_present_trace_contract
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/frame_generation_present_trace_contract_test.py
                    --cxx "${HEAVEN_SURFACE_COLOR_HOST_CXX}"
                    --vulkan-include "${CMAKE_CURRENT_SOURCE_DIR}/../lib/filament/libs/bluevk/include"
                    --work-dir "${CMAKE_CURRENT_BINARY_DIR}/present-trace-contract")
        set_tests_properties(heaven_frame_generation_present_trace_contract PROPERTIES TIMEOUT 120)
        set(HEAVEN_PRESENT_TRACE_HOST_FLAGS)
        if(NOT CMAKE_CROSSCOMPILING)
            separate_arguments(HEAVEN_PRESENT_TRACE_NATIVE_FLAGS NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
            foreach(HEAVEN_PRESENT_TRACE_FLAG IN LISTS HEAVEN_PRESENT_TRACE_NATIVE_FLAGS)
                list(APPEND HEAVEN_PRESENT_TRACE_HOST_FLAGS "--cxx-flag=${HEAVEN_PRESENT_TRACE_FLAG}")
            endforeach()
        endif()
        add_test(NAME heaven_frame_generation_present_trace_concurrency
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/frame_generation_present_trace_concurrency_test.py
                    --cxx "${HEAVEN_SURFACE_COLOR_HOST_CXX}"
                    --compiler-id "${CMAKE_CXX_COMPILER_ID}"
                    --compiler-version "${CMAKE_CXX_COMPILER_VERSION}"
                    ${HEAVEN_PRESENT_TRACE_HOST_FLAGS}
                    --work-dir "${CMAKE_CURRENT_BINARY_DIR}/present-trace-concurrency")
        set_tests_properties(heaven_frame_generation_present_trace_concurrency PROPERTIES TIMEOUT 180)
        add_test(NAME heaven_frame_generation_recovery_contract
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/frame_generation_recovery_contract_test.py
                    --cxx "${HEAVEN_SURFACE_COLOR_HOST_CXX}"
                    --work-dir "${CMAKE_CURRENT_BINARY_DIR}/frame-generation-recovery-contract")
        set_tests_properties(heaven_frame_generation_recovery_contract PROPERTIES TIMEOUT 120)
        add_test(NAME heaven_dlss_frame_token_negative_contract
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/frame_generation/dlss_frame_lifecycle_test.py
                    --cxx "${HEAVEN_SURFACE_COLOR_HOST_CXX}"
                    --work-dir "${CMAKE_CURRENT_BINARY_DIR}/dlss-frame-token-contract")
        set_tests_properties(heaven_dlss_frame_token_negative_contract PROPERTIES TIMEOUT 120)
        add_test(NAME heaven_filament_depth_program_lifetime
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/frame_generation/filament_depth_program_lifetime_test.py
                    --cxx "${HEAVEN_SURFACE_COLOR_HOST_CXX}"
                    --work-dir "${CMAKE_CURRENT_BINARY_DIR}/depth-program-lifetime-test")
        set_tests_properties(heaven_filament_depth_program_lifetime PROPERTIES TIMEOUT 120)
        add_test(NAME heaven_filament_froxel_stream_lifetime
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/frame_generation/filament_froxel_stream_lifetime_test.py
                    --cxx "${HEAVEN_SURFACE_COLOR_HOST_CXX}"
                    --work-dir "${CMAKE_CURRENT_BINARY_DIR}/froxel-stream-lifetime-test")
        set_tests_properties(heaven_filament_froxel_stream_lifetime PROPERTIES TIMEOUT 120)
        add_test(NAME heaven_surface_color_contract
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/surface_color_contract_test.py
                    --materials ${CMAKE_CURRENT_SOURCE_DIR}/materials
                    --cxx "${HEAVEN_SURFACE_COLOR_HOST_CXX}")
        set_tests_properties(heaven_surface_color_contract PROPERTIES TIMEOUT 120)
    endif()
    # The headless source/math contract uses the host shader compiler, not a GPU.
    add_dependencies(heaven_benchmark_tests matc)
    set_tests_properties(heaven_gtao_bent_normal_contract
        heaven_specular_occluded_moment_contract PROPERTIES TIMEOUT 150)
endif()

# Register the renderer-linked smoke harness with CTest too. The executable
# self-gates (exit 77 without HEAVEN_SMOKE=1 / a display), so headless CI
# reports a skip rather than a false rendering pass. Opted-in setup errors
# are failures on a machine that advertises a display.
add_test(NAME heaven_benchmark_smoke COMMAND heaven_benchmark_smoke)
add_test(NAME heaven_temporal_input_contract
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/frame_generation/temporal_input_contract_test.py"
            --cxx "${HEAVEN_SURFACE_COLOR_HOST_CXX}")
set_tests_properties(heaven_benchmark_smoke PROPERTIES
    SKIP_RETURN_CODE 77
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
)
endif() # desktop test executables

# Keep the established host-test build entry point complete as contracts grow.
# Dedicated native GPU fixtures remain explicit, opt-in targets.
if(TARGET heaven_benchmark_tests)
    set(heaven_host_contract_targets
        heaven_profiler_trace_tests
        heaven_capture_camera_motion_tests
        heaven_frame_generation_history_tests
        heaven_frame_generation_present_trace_tests
        heaven_filament_ssr_variant_tests
        heaven_vulkan_pipeline_lifetime_tests
        heaven_dlss_contract_tests
        heaven_dlss_runtime_bridge_tests
        heaven_dlss_frame_token_tests)
    if(HEAVEN_ENABLE_FSR3)
        list(APPEND heaven_host_contract_targets
            heaven_frame_generation_capture_contract_tests
            heaven_fsr31_pipeline_lifetime_test
            heaven_fsr31_view_retirement_test)
    endif()
    add_dependencies(heaven_benchmark_tests ${heaven_host_contract_targets})
endif()
