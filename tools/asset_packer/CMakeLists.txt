cmake_minimum_required(VERSION 3.25)

# This entry point also configures independently, without entering the
# renderer's Filament/SDL/Vulkan build. The same CPU targets back both builds.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(HeavenAssetPacker LANGUAGES C CXX)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/HeavenProjectOptions.cmake)
    set(HEAVEN_CPU_ARCH_LOWER portable)
    set(HEAVEN_PLATFORM_ANDROID ${ANDROID})
    find_path(GLM_INCLUDE_DIR NAMES glm/glm.hpp REQUIRED)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/HeavenCpuDependencies.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/HeavenCpuEngine.cmake)
    set(HEAVEN_BUNDLED_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../benchmark/assets"
        CACHE STRING "Fallback directory for renderer-owned asset packer inputs")
endif()

add_library(heaven_asset_packer_lib STATIC asset_packer.cpp)
set_target_properties(heaven_asset_packer_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(heaven_asset_packer_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(heaven_asset_packer_lib SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/nlohmann)
target_link_libraries(heaven_asset_packer_lib PUBLIC heaven_cpu_engine)
target_compile_definitions(heaven_asset_packer_lib PRIVATE
    HEAVEN_BUNDLED_ASSET_DIR="${HEAVEN_BUNDLED_ASSET_DIR}")
heaven_configure_owned_cpp_target(heaven_asset_packer_lib)
if(NOT MSVC)
    # xml0_parser.h retains TU-local helpers shared with the CPU loaders.
    target_compile_options(heaven_asset_packer_lib PRIVATE -Wno-unused-function)
endif()
if(COMMAND heaven_apply_cpu_arch)
    heaven_apply_cpu_arch(heaven_asset_packer_lib)
endif()

if(NOT HEAVEN_PLATFORM_ANDROID)
    add_executable(heaven_asset_packer main.cpp)
    target_link_libraries(heaven_asset_packer PRIVATE heaven_asset_packer_lib)
    heaven_configure_owned_cpp_target(heaven_asset_packer)
    if(COMMAND heaven_apply_cpu_arch)
        heaven_apply_cpu_arch(heaven_asset_packer)
    endif()
    set_target_properties(heaven_asset_packer PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    install(TARGETS heaven_asset_packer RUNTIME DESTINATION bin)
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../benchmark/assets/
        DESTINATION share/heavenbench/resources)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        enable_testing()
        find_package(Python3 COMPONENTS Interpreter)
        if(Python3_Interpreter_FOUND)
            add_test(NAME heaven_asset_packer_cli
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli.py
                        $<TARGET_FILE:heaven_asset_packer>)
        endif()
    endif()
endif()
