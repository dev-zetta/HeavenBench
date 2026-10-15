cmake_minimum_required(VERSION 3.23)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(HeavenFsr31 LANGUAGES C CXX)
    include(CTest)
endif()

# Intentionally compile only the MIT interpolation/optical-flow components.
# Source is fetched explicitly by scripts/fetch_fsr31.py, never at configure time.
set(HEAVEN_FSR31_SOURCE "" CACHE PATH "Pinned FidelityFX SDK 1.1.4 source")
if(NOT EXISTS "${HEAVEN_FSR31_SOURCE}/sdk/src/backends/vk/ffx_vk.cpp")
    message(FATAL_ERROR "Fetch pinned SDK with scripts/fetch_fsr31.py and set HEAVEN_FSR31_SOURCE")
endif()
find_package(Vulkan REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(HEAVEN_FSR_GLSLANG glslangValidator REQUIRED)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_fsr31_source.py"
    --sdk "${HEAVEN_FSR31_SOURCE}" --lock "${CMAKE_CURRENT_SOURCE_DIR}/fsr31-source.sha256"
    RESULT_VARIABLE fsr_verify_result OUTPUT_VARIABLE fsr_verify_output ERROR_VARIABLE fsr_verify_error)
if(NOT fsr_verify_result EQUAL 0)
    message(FATAL_ERROR "Pinned FSR source verification failed: ${fsr_verify_output}${fsr_verify_error}")
endif()
message(STATUS "${fsr_verify_output}")

set(fsr_sdk "${HEAVEN_FSR31_SOURCE}/sdk")
set(reflect_dir "${fsr_sdk}/tools/ffx_shader_compiler/libs/SPIRV-Reflect")
add_executable(heaven_fsr31_reflect spirv_reflect_json.cpp "${reflect_dir}/spirv_reflect.c")
target_include_directories(heaven_fsr31_reflect PRIVATE "${reflect_dir}")
target_compile_features(heaven_fsr31_reflect PRIVATE cxx_std_17)

file(GLOB_RECURSE fsr_shader_inputs CONFIGURE_DEPENDS "${fsr_sdk}/include/FidelityFX/gpu/*.h" "${fsr_sdk}/src/backends/vk/shaders/frameinterpolation/*.glsl" "${fsr_sdk}/src/backends/vk/shaders/opticalflow/*.glsl")
set(fsr_shader_blobs "${CMAKE_CURRENT_BINARY_DIR}/fsr31_shader_blobs.cpp")
add_custom_command(OUTPUT "${fsr_shader_blobs}"
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/generate_fsr31_shaders.py"
        --sdk "${HEAVEN_FSR31_SOURCE}" --output "${fsr_shader_blobs}"
        --compiler "${HEAVEN_FSR_GLSLANG}" --reflect "$<TARGET_FILE:heaven_fsr31_reflect>"
    DEPENDS heaven_fsr31_reflect generate_fsr31_shaders.py ${fsr_shader_inputs}
    VERBATIM)

# The compute provider deliberately does not use AMD's Windows-only swapchain.
# Remove its single callback assignment in a generated copy, with exact match.
file(READ "${fsr_sdk}/src/backends/vk/ffx_vk.cpp" fsr_vk_source)
set(fsr_swapchain_assignment "backendInterface->fpSwapChainConfigureFrameGeneration = ffxSetFrameGenerationConfigToSwapchainVK;")
string(FIND "${fsr_vk_source}" "${fsr_swapchain_assignment}" assignment_position)
if(assignment_position EQUAL -1)
    message(FATAL_ERROR "Pinned SDK Vulkan source changed; review source transform")
endif()
string(REPLACE "${fsr_swapchain_assignment}" "backendInterface->fpSwapChainConfigureFrameGeneration = nullptr;" fsr_vk_source "${fsr_vk_source}")
set(fsr_requirements_assignment "backendContext->vkFunctionTable.vkGetBufferMemoryRequirements2KHR = (PFN_vkGetBufferMemoryRequirements2KHR)vkDeviceContext->vkDeviceProcAddr(backendContext->device, \"vkGetBufferMemoryRequirements2KHR\");")
string(FIND "${fsr_vk_source}" "${fsr_requirements_assignment}" requirements_position)
if(requirements_position EQUAL -1)
    message(FATAL_ERROR "Pinned SDK memory-requirements loader changed; review core alias fallback")
endif()
string(REPLACE "${fsr_requirements_assignment}" "${fsr_requirements_assignment}\n        if (!backendContext->vkFunctionTable.vkGetBufferMemoryRequirements2KHR) backendContext->vkFunctionTable.vkGetBufferMemoryRequirements2KHR = (PFN_vkGetBufferMemoryRequirements2KHR)vkDeviceContext->vkDeviceProcAddr(backendContext->device, \"vkGetBufferMemoryRequirements2\");" fsr_vk_source "${fsr_vk_source}")
# The upstream packed scratch mapping does not align its alignas(32) context
# array. Clang legitimately emits aligned stores and faults at the old offset.
set(fsr_context_mapping "backendContext->pEffectContexts = (BackendContext_VK::EffectContext*)pMem;")
set(fsr_scratch_tail "pipelineArraySize + resourceArraySize + contextArraySize,")
foreach(fsr_exact IN ITEMS fsr_context_mapping fsr_scratch_tail)
    string(FIND "${fsr_vk_source}" "${${fsr_exact}}" alignment_position)
    if(alignment_position EQUAL -1)
        message(FATAL_ERROR "Pinned SDK scratch layout changed; review context alignment")
    endif()
endforeach()
string(REPLACE "${fsr_context_mapping}" "pMem = reinterpret_cast<uint8_t*>(FFX_ALIGN_UP(reinterpret_cast<uintptr_t>(pMem), alignof(BackendContext_VK::EffectContext)));\n        ${fsr_context_mapping}" fsr_vk_source "${fsr_vk_source}")
string(REPLACE "${fsr_scratch_tail}" "pipelineArraySize + resourceArraySize + contextArraySize + alignof(BackendContext_VK::EffectContext) - 1," fsr_vk_source "${fsr_vk_source}")
# Enumerating physical memory does not establish that AMD's optional logical
# device feature was enabled. This compute provider never requests it, so keep
# every allocation path on ordinary memory types, including unused breadcrumbs.
set(fsr_memory_match "if ((memRequirements.memoryTypeBits & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & requestedProperties)) {")
set(fsr_breadcrumb_flags "const VkMemoryPropertyFlags memFlags = memoryProps.memoryTypes[i].propertyFlags;")
set(fsr_coherent_capability "deviceCapabilities->deviceCoherentMemorySupported = (bool)coherentMemoryFeatures.deviceCoherentMemory;")
# The pinned array has six entries; the literal five omits STORAGE_BUFFER.
set(fsr_descriptor_pool_count "descriptorPoolCreateInfo.poolSizeCount = 5;")
foreach(fsr_exact IN ITEMS fsr_memory_match fsr_breadcrumb_flags fsr_coherent_capability fsr_descriptor_pool_count)
    string(FIND "${fsr_vk_source}" "${${fsr_exact}}" correction_position)
    if(correction_position EQUAL -1)
        message(FATAL_ERROR "Pinned SDK Vulkan allocation contract changed; review ${fsr_exact}")
    endif()
endforeach()
string(REPLACE "${fsr_memory_match}" "const VkMemoryPropertyFlags memoryFlags = memProperties.memoryTypes[i].propertyFlags;\n        if (memoryFlags & (VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD)) continue;\n        if ((memRequirements.memoryTypeBits & (1u << i)) && (memoryFlags & requestedProperties) == requestedProperties) {" fsr_vk_source "${fsr_vk_source}")
string(REPLACE "${fsr_breadcrumb_flags}" "${fsr_breadcrumb_flags}\n                    if (memFlags & (VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD)) continue;" fsr_vk_source "${fsr_vk_source}")
string(REPLACE "${fsr_coherent_capability}" "deviceCapabilities->deviceCoherentMemorySupported = false; // Not enabled on the borrowed logical device." fsr_vk_source "${fsr_vk_source}")
string(REPLACE "${fsr_descriptor_pool_count}" "descriptorPoolCreateInfo.poolSizeCount = static_cast<uint32_t>(sizeof(poolSizes) / sizeof(poolSizes[0]));" fsr_vk_source "${fsr_vk_source}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ffx_vk_compute.cpp" CONTENT "${fsr_vk_source}" @ONLY)
# Retire partial pipeline/global setup and keep FI Prepare views owned until
# final Dispatch. Transform only the generated copy and reject source drift.
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/transform_fsr31_backend.py"
    --input "${CMAKE_CURRENT_BINARY_DIR}/ffx_vk_compute.cpp"
    --output "${CMAKE_CURRENT_BINARY_DIR}/ffx_vk_compute.cpp"
    RESULT_VARIABLE fsr_pipeline_transform_result ERROR_VARIABLE fsr_pipeline_transform_error)
if(NOT fsr_pipeline_transform_result EQUAL 0)
    message(FATAL_ERROR "Pinned SDK backend correction transform failed: ${fsr_pipeline_transform_error}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS transform_fsr31_backend.py)
# The CPU fixtures intentionally include the exact private implementation to
# exercise its failure paths. Give that include a conventional .inc extension;
# COPYONLY keeps it byte-identical to the backend compiled into the SDK target.
configure_file("${CMAKE_CURRENT_BINARY_DIR}/ffx_vk_compute.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/ffx_vk_compute_test.inc" COPYONLY)

# Unix wchar_t is four bytes; the SDK's numerous wide descriptor names make
# its private FI context larger than the Windows-sized opaque reservation.
# Preserve the SDK's compile-time size oracle and widen only the reservation.
set(fsr_overlay "${CMAKE_CURRENT_BINARY_DIR}/include")
if(NOT WIN32)
    file(READ "${fsr_sdk}/include/FidelityFX/host/ffx_types.h" fsr_types)
    string(FIND "${fsr_types}" "#define FFX_SDK_DEFAULT_CONTEXT_SIZE (1024 * 128)" context_position)
    if(context_position EQUAL -1)
        message(FATAL_ERROR "Pinned SDK context definition changed; review Unix ABI reservation")
    endif()
    string(REPLACE "#define FFX_SDK_DEFAULT_CONTEXT_SIZE (1024 * 128)" "#define FFX_SDK_DEFAULT_CONTEXT_SIZE (1024 * 256)" fsr_types "${fsr_types}")
    file(CONFIGURE OUTPUT "${fsr_overlay}/FidelityFX/host/ffx_types.h" CONTENT "${fsr_types}" @ONLY)
endif()

add_library(heaven_fsr31_sdk STATIC
    "${CMAKE_CURRENT_BINARY_DIR}/ffx_vk_compute.cpp"
    "${fsr_shader_blobs}"
    "${fsr_sdk}/src/components/frameinterpolation/ffx_frameinterpolation.cpp"
    "${fsr_sdk}/src/components/opticalflow/ffx_opticalflow.cpp"
    "${fsr_sdk}/src/shared/ffx_assert.cpp"
    "${fsr_sdk}/src/shared/ffx_message.cpp"
    "${fsr_sdk}/src/shared/ffx_object_management.cpp"
    "${fsr_sdk}/src/shared/ffx_breadcrumbs_list.cpp")
target_include_directories(heaven_fsr31_sdk BEFORE PUBLIC "${fsr_overlay}" "${fsr_sdk}/include" PRIVATE
    "${fsr_sdk}/src/shared" "${fsr_sdk}/src/backends/shared")
target_compile_features(heaven_fsr31_sdk PRIVATE cxx_std_17)
target_link_libraries(heaven_fsr31_sdk PUBLIC Vulkan::Vulkan)
if(NOT WIN32)
    target_compile_options(heaven_fsr31_sdk PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/fsr31_portability.h")
endif()
set_target_properties(heaven_fsr31_sdk PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(heaven_fsr31_provider STATIC fsr31_vulkan.cpp)
target_include_directories(heaven_fsr31_provider PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(heaven_fsr31_provider PUBLIC cxx_std_23)
target_link_libraries(heaven_fsr31_provider PUBLIC heaven_fsr31_sdk)
install(FILES "${HEAVEN_FSR31_SOURCE}/LICENSE.txt"
    DESTINATION share/licenses/HeavenBench RENAME FidelityFX-SDK.txt)
install(FILES "${reflect_dir}/LICENSE"
    DESTINATION share/licenses/HeavenBench RENAME SPIRV-Reflect.txt)

# Exercise the actual copied backend's allocation failure paths with fake
# Vulkan calls. This executable never creates a device or submits GPU work.
add_executable(heaven_fsr31_pipeline_lifetime_test fsr31_pipeline_lifetime_test.cpp)
target_compile_definitions(heaven_fsr31_pipeline_lifetime_test PRIVATE
    HEAVEN_FSR31_TEST_BACKEND_COPY="${CMAKE_CURRENT_BINARY_DIR}/ffx_vk_compute_test.inc")
target_include_directories(heaven_fsr31_pipeline_lifetime_test PRIVATE
    "${fsr_sdk}/src/shared" "${fsr_sdk}/src/backends/shared")
target_compile_features(heaven_fsr31_pipeline_lifetime_test PRIVATE cxx_std_23)
if(NOT WIN32)
    target_compile_options(heaven_fsr31_pipeline_lifetime_test PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/fsr31_portability.h")
endif()
target_link_libraries(heaven_fsr31_pipeline_lifetime_test PRIVATE heaven_fsr31_sdk)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND BUILD_TESTING)
    add_test(NAME heaven_fsr31_pipeline_lifetime_test COMMAND heaven_fsr31_pipeline_lifetime_test)
endif()


# Source-aware CPU lifetime regression: actual generated registration/unregister
# code and the same prepare-scope guard used by the provider; no GPU execution.
add_executable(heaven_fsr31_view_retirement_test fsr31_view_retirement_test.cpp)
target_compile_definitions(heaven_fsr31_view_retirement_test PRIVATE
    HEAVEN_FSR31_TEST_BACKEND_COPY="${CMAKE_CURRENT_BINARY_DIR}/ffx_vk_compute_test.inc")
target_include_directories(heaven_fsr31_view_retirement_test PRIVATE
    "${fsr_sdk}/src/shared" "${fsr_sdk}/src/backends/shared")
target_compile_features(heaven_fsr31_view_retirement_test PRIVATE cxx_std_17)
if(NOT WIN32)
    target_compile_options(heaven_fsr31_view_retirement_test PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/fsr31_portability.h")
endif()
target_link_libraries(heaven_fsr31_view_retirement_test PRIVATE heaven_fsr31_sdk)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND BUILD_TESTING)
    add_test(NAME heaven_fsr31_view_retirement_test COMMAND heaven_fsr31_view_retirement_test)
    foreach(view_negative IN ITEMS old-double-rotation unbounded-inflight)
        add_test(NAME heaven_fsr31_view_retirement_${view_negative}
            COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/check_fsr31_view_negative.py"
                --executable "$<TARGET_FILE:heaven_fsr31_view_retirement_test>" --case "${view_negative}")
    endforeach()
endif()

option(HEAVEN_FSR31_NATIVE_TEST "Build the opt-in real Vulkan FSR component fixture" OFF)
if(HEAVEN_FSR31_NATIVE_TEST)
    add_executable(heaven_fsr31_native_test fsr31_vulkan_test.cpp)
    target_link_libraries(heaven_fsr31_native_test PRIVATE heaven_fsr31_provider)
endif()
