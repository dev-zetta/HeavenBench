# Optional compute diagnostics; never linked into the scene renderer or its score.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(HEAVEN_GPU_LAB_GLSLANG NAMES glslangValidator REQUIRED)
find_program(HEAVEN_GPU_LAB_SPIRV_VAL NAMES spirv-val REQUIRED)
set(gpu_lab_modules)
foreach(kernel reference cooperative int8_reference int8_dp4a int8_cooperative int8_zero)
    set(module "${CMAKE_CURRENT_BINARY_DIR}/${kernel}.spv")
    add_custom_command(OUTPUT "${module}"
        COMMAND "${HEAVEN_GPU_LAB_GLSLANG}" -V --target-env vulkan1.3
                "${CMAKE_CURRENT_SOURCE_DIR}/${kernel}.comp" -o "${module}"
        COMMAND "${HEAVEN_GPU_LAB_SPIRV_VAL}" --target-env vulkan1.3 "${module}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${kernel}.comp" VERBATIM)
    list(APPEND gpu_lab_modules "${module}")
endforeach()
set(gpu_lab_header "${CMAKE_CURRENT_BINARY_DIR}/gpu_lab_shaders.h")
add_custom_command(OUTPUT "${gpu_lab_header}"
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/embed_shaders.py"
            ${gpu_lab_modules} "${gpu_lab_header}"
    DEPENDS ${gpu_lab_modules} "${CMAKE_CURRENT_SOURCE_DIR}/embed_shaders.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/int8_contract.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt" VERBATIM)
add_executable(heaven_gpu_lab main.cpp "${gpu_lab_header}")
heaven_configure_owned_cpp_target(heaven_gpu_lab)
target_include_directories(heaven_gpu_lab PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/nlohmann")
target_link_libraries(heaven_gpu_lab PRIVATE Vulkan::Vulkan)
target_compile_definitions(heaven_gpu_lab PRIVATE HEAVEN_GPU_LAB_VERSION="${PROJECT_VERSION}"
    HEAVEN_GPU_LAB_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
add_executable(heaven_gpu_lab_int8_contract_tests int8_contract_test.cpp)
heaven_configure_owned_cpp_target(heaven_gpu_lab_int8_contract_tests)
add_test(NAME heaven_gpu_lab_int8_contract COMMAND heaven_gpu_lab_int8_contract_tests)
add_dependencies(heaven_gpu_lab heaven_gpu_lab_int8_contract_tests)
