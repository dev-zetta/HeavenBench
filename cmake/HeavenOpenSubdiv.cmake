include_guard(GLOBAL)

# Unmodified pinned OpenSubdiv v3_6_1 CPU topology and patch evaluation only.
add_library(heaven_opensubdiv_cpu STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/far/bilinearPatchBuilder.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/far/catmarkPatchBuilder.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/far/error.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/far/loopPatchBuilder.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/far/patchBasis.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/far/patchBuilder.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/far/patchDescriptor.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/far/patchMap.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/far/patchTable.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/far/patchTableFactory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/far/ptexIndices.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/far/stencilBuilder.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/far/stencilTable.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/far/stencilTableFactory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/far/topologyDescriptor.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/far/topologyRefiner.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/far/topologyRefinerFactory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/sdc/crease.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/sdc/typeTraits.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/vtr/fvarLevel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/vtr/fvarRefinement.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/vtr/level.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/vtr/quadRefinement.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/vtr/refinement.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/vtr/sparseSelector.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv/opensubdiv/vtr/triRefinement.cpp
)
set_target_properties(heaven_opensubdiv_cpu PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES POSITION_INDEPENDENT_CODE ON)
target_include_directories(heaven_opensubdiv_cpu SYSTEM PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/../lib/OpenSubdiv)
if(MSVC)
    # Upstream's public scheme templates use the MSVC math constants too.
    target_compile_definitions(heaven_opensubdiv_cpu PUBLIC _USE_MATH_DEFINES)
endif()
