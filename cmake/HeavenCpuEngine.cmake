include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/HeavenOpenSubdiv.cmake)
get_filename_component(HEAVEN_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Compile CPU asset loaders once for the renderer, standalone packer, and tests.
# These sources depend on the abstract RHI only; no graphics library is linked.
set(HEAVEN_CPU_ENGINE_SOURCES
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/asset_file_system.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/dds_block_codec.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/gpu_driver_info.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/ground_relief_generator.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/surface_displacement_policy.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/zip_archive.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/tessellation_cache_archive.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/cli_parser.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/config_file.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/render_presets.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/tessellation_levels.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/render_test_scene_catalog.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/render_test_scene.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/path_resolver.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/runtime_paths.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/mesh.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/material_rhi.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/scene_rhi.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/staged_scene_load.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/null_backend.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/asset_usage_recorder.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/camera_path.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/skinned_mesh.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/rope_sim.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/grass_cpu.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/bounded_displacement_surface.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/displacement_surface_priority.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/spatial_draw_partition.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/cpu_tessellation.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/closed_displacement_seams.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/cloth_sim.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/engine/camera_collision.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/benchmark/scene_markers.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/benchmark/microprofiler.cpp
    ${HEAVEN_REPO_ROOT}/benchmark/src/benchmark/microprofile_report.cpp
)
add_library(heaven_cpu_engine STATIC ${HEAVEN_CPU_ENGINE_SOURCES})
set_target_properties(heaven_cpu_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(heaven_cpu_engine PUBLIC ${HEAVEN_REPO_ROOT}/benchmark/src)
target_include_directories(heaven_cpu_engine SYSTEM PUBLIC ${GLM_INCLUDE_DIR})
target_include_directories(heaven_cpu_engine SYSTEM PRIVATE
    ${HEAVEN_REPO_ROOT}/lib/JoltPhysics
    ${HEAVEN_REPO_ROOT}/lib/pocketfft
    ${HEAVEN_REPO_ROOT}/lib/bcdec
    ${HEAVEN_REPO_ROOT}/lib/filament/third_party/stb
    ${HEAVEN_REPO_ROOT}/lib/nlohmann)
target_link_libraries(heaven_cpu_engine PUBLIC Jolt libzip::zip heaven_opensubdiv_cpu)
heaven_configure_owned_cpp_target(heaven_cpu_engine)
if(COMMAND heaven_apply_cpu_arch)
    heaven_apply_cpu_arch(heaven_cpu_engine)
endif()
if(NOT MSVC)
    set_source_files_properties(
        ${HEAVEN_REPO_ROOT}/benchmark/src/engine/asset_file_system.cpp
        ${HEAVEN_REPO_ROOT}/benchmark/src/engine/zip_archive.cpp
        ${HEAVEN_REPO_ROOT}/benchmark/src/engine/tessellation_cache_archive.cpp
        PROPERTIES COMPILE_OPTIONS "-Wno-nullability-extension")
    target_compile_options(heaven_cpu_engine PRIVATE
        -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers)
endif()
