include_guard(GLOBAL)

option(HEAVEN_ENABLE_CLANG_TIDY
       "Run clang-tidy while compiling project-owned C++ targets" OFF)

function(heaven_configure_owned_cpp_target target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Unknown project-owned target: ${target}")
    endif()

    target_compile_features("${target}" PRIVATE cxx_std_23)
    set_target_properties("${target}" PROPERTIES
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options("${target}" PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-Wall>
            $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
            $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>
            $<$<COMPILE_LANGUAGE:CXX>:-Wimplicit-fallthrough>
            $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
            $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>
            $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>
            $<$<COMPILE_LANGUAGE:CXX>:-Wshadow>
        )
    elseif(MSVC)
        target_compile_options("${target}" PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:/W4>
            $<$<COMPILE_LANGUAGE:CXX>:/permissive->
        )
    endif()

    if(HEAVEN_ENABLE_CLANG_TIDY)
        find_program(HEAVEN_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
        set_property(TARGET "${target}" PROPERTY CXX_CLANG_TIDY
            "${HEAVEN_CLANG_TIDY_EXECUTABLE};--quiet"
        )
    endif()
endfunction()
