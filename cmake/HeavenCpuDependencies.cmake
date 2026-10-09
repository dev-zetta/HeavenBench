include_guard(GLOBAL)
get_filename_component(HEAVEN_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# ── libzip ───────────────────────────────────────────────────────
# Runtime assets and the tessellation cache use ZIP64 archives through the
# pinned libzip checkout populated by scripts/fetch_dependencies.sh. Keep the
# dependency minimal and static: HeavenBench needs only the baseline DEFLATE
# codec supplied by zlib.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_REGRESS OFF CACHE BOOL "" FORCE)
set(BUILD_OSSFUZZ OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_DOC OFF CACHE BOOL "" FORCE)
set(LIBZIP_DO_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_COMMONCRYPTO OFF CACHE BOOL "" FORCE)
set(ENABLE_GNUTLS OFF CACHE BOOL "" FORCE)
set(ENABLE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
set(ENABLE_WINDOWS_CRYPTO OFF CACHE BOOL "" FORCE)
set(ENABLE_BZIP2 OFF CACHE BOOL "" FORCE)
set(ENABLE_LZMA OFF CACHE BOOL "" FORCE)
set(ENABLE_ZSTD OFF CACHE BOOL "" FORCE)
add_subdirectory(${HEAVEN_REPO_ROOT}/lib/libzip
                 ${CMAKE_CURRENT_BINARY_DIR}/libzip)

# ── Jolt Physics ─────────────────────────────────────────────────
# Build Jolt as a static library from the local clone
set(DOUBLE_PRECISION OFF)
set(GENERATE_DEBUG_SYMBOLS ON)
set(OVERRIDE_CXX_FLAGS OFF)       # Don't let Jolt override our flags
set(CROSS_PLATFORM_DETERMINISTIC OFF)
set(INTERPROCEDURAL_OPTIMIZATION OFF)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF)
set(CPP_EXCEPTIONS_ENABLED ON)
set(CPP_RTTI_ENABLED ON)
set(OBJECT_LAYER_BITS 16)
# Jolt publishes its instruction flags to consumers because its inline headers
# must use the same ABI. Keep the portable renderer at the x86-64 baseline;
# explicit native/Zen variants opt into the wider Jolt implementation together
# with the renderer's matching -march selection above.
if(HEAVEN_PLATFORM_ANDROID OR
   HEAVEN_CPU_ARCH_LOWER STREQUAL "portable")
    set(USE_SSE4_1 OFF)
    set(USE_SSE4_2 OFF)
    set(USE_AVX OFF)
    set(USE_AVX2 OFF)
else()
    set(USE_SSE4_1 ON)
    set(USE_SSE4_2 ON)
    set(USE_AVX ON)
    set(USE_AVX2 ON)
endif()
set(USE_AVX512 OFF)
if(HEAVEN_PLATFORM_ANDROID OR
   HEAVEN_CPU_ARCH_LOWER STREQUAL "portable")
    set(USE_LZCNT OFF)
    set(USE_TZCNT OFF)
    set(USE_F16C OFF)
    set(USE_FMADD OFF)
else()
    set(USE_LZCNT ON)
    set(USE_TZCNT ON)
    set(USE_F16C ON)
    set(USE_FMADD ON)
endif()
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)        # Disable Vulkan compute (needs glslc)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)      # Disable DirectX compute
set(JPH_USE_METAL OFF CACHE BOOL "" FORCE)     # Disable Metal compute
set(JPH_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(JPH_BUILD_JOLT_VIEWER OFF CACHE BOOL "" FORCE)
set(JPH_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(JPH_BUILD_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)

add_subdirectory(${HEAVEN_REPO_ROOT}/lib/JoltPhysics/Build ${CMAKE_CURRENT_BINARY_DIR}/JoltPhysics)
set_target_properties(Jolt PROPERTIES SYSTEM TRUE)
