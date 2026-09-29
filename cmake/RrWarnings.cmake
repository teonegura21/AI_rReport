# Compiler warnings for rReport's own targets.
#
# Applied per target (never globally) so third-party code built through
# FetchContent is not held to our standard. Dependency headers are included as
# SYSTEM headers (imported targets, FetchContent SYSTEM), so their warnings are
# silenced too.
include_guard(GLOBAL)

set(_rr_gcc_clang_warnings
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Woverloaded-virtual
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    -Wunused)

set(_rr_gcc_only_warnings
    -Wmisleading-indentation
    -Wduplicated-cond
    -Wduplicated-branches
    -Wlogical-op)

# rr_target_warnings(<target>)
#   Adds the project warning set to <target>'s own sources.
function(rr_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(RR_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        return()
    endif()

    target_compile_options(${target} PRIVATE ${_rr_gcc_clang_warnings})
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE ${_rr_gcc_only_warnings})
    endif()
    if(RR_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()
