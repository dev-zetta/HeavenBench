# Optional native GPU oracle, included after heaven_benchmark is configured.
# Explicit build/run only: ordinary host CTest must not require a GPU device.
if(NOT HEAVEN_PLATFORM_ANDROID AND TARGET heaven_benchmark)
    function(heaven_add_native_frame_generation_test target source)
        add_executable(${target} EXCLUDE_FROM_ALL
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../benchmark/src/rhi/frame_generation/${source}"
            ${ENGINE_SOURCES} ${RHI_SOURCES} ${BENCHMARK_SOURCES})
        foreach(property IN ITEMS
                INCLUDE_DIRECTORIES SYSTEM_INCLUDE_DIRECTORIES
                COMPILE_DEFINITIONS COMPILE_OPTIONS
                LINK_LIBRARIES LINK_OPTIONS LINK_DIRECTORIES
                CXX_STANDARD CXX_STANDARD_REQUIRED CXX_EXTENSIONS)
            get_target_property(value heaven_benchmark "${property}")
            if(NOT "${value}" MATCHES "-NOTFOUND$")
                set_property(TARGET ${target} PROPERTY "${property}" "${value}")
            endif()
        endforeach()
    endfunction()
    heaven_add_native_frame_generation_test(heaven_temporal_scene_test temporal_scene_test.cpp)
    heaven_add_native_frame_generation_test(heaven_frame_generation_output_test frame_generation_output_test.cpp)
endif()
