include(CheckIPOSupported)

# Per-compiler optimization flags for the main library target
function(deflate_set_compiler_flags target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            $<$<CONFIG:Release>:/O2 /Oi /GL /Gy>
            $<$<CONFIG:RelWithDebInfo>:/O2 /Oi>
            /W4
        )
        target_link_options(${target} PRIVATE
            $<$<CONFIG:Release>:/LTCG>
        )
    else()
        target_compile_options(${target} PRIVATE
            $<$<CONFIG:Release>:-O3 -ffast-math -funroll-loops -fomit-frame-pointer>
            $<$<CONFIG:RelWithDebInfo>:-O2 -g>
            $<$<CONFIG:Debug>:-O0 -g>
            -Wall -Wextra -Wno-unused-parameter
        )
    endif()

    # LTO for release builds
    check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_OUTPUT)
    if(IPO_SUPPORTED)
        set_property(TARGET ${target} PROPERTY
            INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()
endfunction()
