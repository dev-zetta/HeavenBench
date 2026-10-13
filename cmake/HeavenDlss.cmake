# Optional SDK source is prepared explicitly, never fetched at configure time.
option(HEAVEN_ENABLE_DLSS "Experimental Windows Vulkan DLSS frame generation" OFF)
set(HEAVEN_STREAMLINE_HEADERS "" CACHE PATH "Verified Streamline 2.14.1 header root")
if(HEAVEN_ENABLE_DLSS)
    if(NOT HEAVEN_PLATFORM_WINDOWS)
        message(FATAL_ERROR "DLSS frame generation requires the Windows Vulkan Streamline build")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/frame_generation/dlss_fetch_sdk.py"
            --output "${HEAVEN_STREAMLINE_HEADERS}" --verify-only
        RESULT_VARIABLE dlss_verify_result
        OUTPUT_VARIABLE dlss_verify_output ERROR_VARIABLE dlss_verify_error)
    if(NOT dlss_verify_result EQUAL 0)
        message(FATAL_ERROR "Streamline provenance verification failed: ${dlss_verify_output}${dlss_verify_error}")
    endif()
    message(STATUS "Verified Streamline 2.14.1 headers: ${dlss_verify_output}")
endif()

add_library(heaven_dlss_provider STATIC
    src/rhi/frame_generation/dlss_provider.cpp
    src/rhi/frame_generation/dlss_runtime_loader.cpp)
target_include_directories(heaven_dlss_provider PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/frame_generation")
# The public adapter uses Vulkan types even with the SDK disabled. Propagate
# the desktop header-only dependency; Android uses its existing NDK sysroot.
if(HEAVEN_VULKAN_LINK)
    target_link_libraries(heaven_dlss_provider PUBLIC ${HEAVEN_VULKAN_LINK})
endif()
target_compile_definitions(heaven_dlss_provider PRIVATE
    HEAVEN_HAS_STREAMLINE=$<BOOL:${HEAVEN_ENABLE_DLSS}>)
set_target_properties(heaven_dlss_provider PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(HEAVEN_ENABLE_DLSS)
    # The pinned SDK has an invalid C++23 function alias in sl_pcl.h. Keep its
    # headers untouched and isolate the two adapter units at C++20.
    set_target_properties(heaven_dlss_provider PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(heaven_dlss_provider SYSTEM PRIVATE
        "${HEAVEN_STREAMLINE_HEADERS}/include")
    install(FILES "${HEAVEN_STREAMLINE_HEADERS}/license.txt"
        DESTINATION share/licenses/HeavenBench RENAME Streamline.txt)
    if(MSVC)
        target_compile_options(heaven_dlss_provider PRIVATE /W4 /permissive-)
    else()
        target_compile_options(heaven_dlss_provider PRIVATE -Wall -Wextra -Wpedantic)
    endif()
else()
    heaven_configure_owned_cpp_target(heaven_dlss_provider)
endif()

list(APPEND RHI_SOURCES
    src/rhi/frame_generation/dlss_platform.cpp
    src/rhi/frame_generation/dlss_runtime_bridge.cpp)

function(heaven_link_dlss target)
    target_compile_definitions(${target} PRIVATE
        HEAVEN_HAS_DLSS=$<BOOL:${HEAVEN_ENABLE_DLSS}>
        HEAVENBENCH_VERSION="${PROJECT_VERSION}")
    target_link_libraries(${target} PRIVATE heaven_dlss_provider)
endfunction()
