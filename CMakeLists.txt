cmake_minimum_required(VERSION 3.25)
project(uniginex VERSION 0.3.1 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/HeavenProjectOptions.cmake)

# Find zlib (required)
find_package(ZLIB REQUIRED)

# lz4 is compiled from source (bundled)
add_library(lz4 STATIC lib/lz4/lz4.c)
target_include_directories(lz4 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lib/lz4)

# Main executable
add_executable(uniginex tools/extraction/uniginex.cpp)
target_link_libraries(uniginex PRIVATE lz4 ZLIB::ZLIB)
heaven_configure_owned_cpp_target(uniginex)

# Mesh converter tool
add_executable(mesh2obj tools/extraction/mesh2obj.cpp)
heaven_configure_owned_cpp_target(mesh2obj)

# Install targets
install(TARGETS uniginex mesh2obj RUNTIME DESTINATION bin)

# Tests
enable_testing()

# Tool-level test for the mesh2obj per-surface count guard: a crafted
# bad-count mi08 buffer must exit non-zero without crashing or writing an OBJ.
add_test(
    NAME mesh2obj_bad_count
    COMMAND ${CMAKE_COMMAND} -E env sh
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh2obj_bad_count_test.sh
            $<TARGET_FILE:mesh2obj>
)

add_test(
    NAME uniginex_invalid_input
    COMMAND ${CMAKE_COMMAND} -E env sh
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/uniginex_invalid_input_test.sh
        $<TARGET_FILE:uniginex>
)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME visual_audit_regressions
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/visual_audit_test.py
    )
    add_test(
        NAME docker_cache_regressions
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/docker_cache_test.py
    )
    add_test(
        NAME docker_build_regressions
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/docker_build_test.py
    )
    add_test(
        NAME dependency_bootstrap_regressions
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/dependency_bootstrap_test.py
    )
    add_test(
        NAME asset_builder_regressions
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/asset_builder_test.py
    )
    add_test(
        NAME repository_hygiene
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/repository_hygiene_test.py
    )
    add_test(
        NAME extraction_regressions
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/extraction_regression_test.py
                $<TARGET_FILE:uniginex> $<TARGET_FILE:mesh2obj>
    )
    add_test(
        NAME assemble_scene_regressions
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/assemble_scene_test.py
    )
endif()
