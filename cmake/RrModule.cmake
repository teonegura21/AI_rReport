# Module, executable and test-suite helpers.
#
# The central idea: a module's *only* public surface is src/<module>/include/,
# and include paths propagate exclusively through target_link_libraries(). A
# module therefore cannot include a header of a module it does not link — the
# compiler enforces the architecture's layering, not code review.
include_guard(GLOBAL)

# Applies the project's static analysis to one of *our* targets. Dependencies
# built through FetchContent are deliberately excluded: their style is not ours
# to enforce, and their findings would drown out real ones.
function(_rr_apply_clang_tidy target)
    if(RR_CLANG_TIDY)
        set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${RR_CLANG_TIDY}")
    endif()
endfunction()

# rr_add_module(<name>
#     [SOURCES <files...>]          # omit for header-only modules
#     [PUBLIC_DEPS <targets...>]    # visible in this module's public headers
#     [PRIVATE_DEPS <targets...>])  # implementation-only (e.g. hidden third-party libs)
#
# Creates the static library `rr_<name>` (alias `rr::<name>`), rooted in the
# calling directory, with public include directory `<dir>/include`.
function(rr_add_module name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS")
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "rr_add_module(${name}): unknown arguments ${ARG_UNPARSED_ARGUMENTS}")
    endif()

    set(target rr_${name})
    if(ARG_SOURCES)
        add_library(${target} STATIC ${ARG_SOURCES})
        target_include_directories(${target}
            PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(${target} PUBLIC ${ARG_PUBLIC_DEPS} PRIVATE ${ARG_PRIVATE_DEPS})
        target_compile_features(${target} PUBLIC cxx_std_23)
        rr_target_warnings(${target})
        _rr_apply_clang_tidy(${target})
    else()
        add_library(${target} INTERFACE)
        target_include_directories(${target}
            INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
        target_link_libraries(${target} INTERFACE ${ARG_PUBLIC_DEPS})
        target_compile_features(${target} INTERFACE cxx_std_23)
    endif()
    add_library(rr::${name} ALIAS ${target})
endfunction()

# rr_add_executable(<name> SOURCES <files...> [DEPS <targets...>])
#   An application binary. Apps are thin: they parse arguments and wire modules.
function(rr_add_executable name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;DEPS")
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE ${ARG_DEPS})
    target_compile_features(${name} PRIVATE cxx_std_23)
    rr_target_warnings(${name})
    _rr_apply_clang_tidy(${name})
endfunction()

# rr_add_test_suite(<module> SOURCES <files...> [DEPS <targets...>])
#   One GoogleTest executable per module (`rr_<module>_tests`), registered with
#   CTest and labelled with the module name (`ctest -L core`). Every suite links
#   rr::testkit (tests/testkit), the helpers shared by all tests.
#   DISCOVERY_MODE PRE_TEST lists tests when CTest runs, not at build time, which
#   keeps sanitizer builds and cross builds working.
function(rr_add_test_suite module)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;DEPS")
    set(target rr_${module}_tests)
    add_executable(${target} ${ARG_SOURCES})
    target_link_libraries(${target}
        PRIVATE ${ARG_DEPS} rr::testkit GTest::gmock GTest::gtest GTest::gtest_main)
    target_compile_features(${target} PRIVATE cxx_std_23)
    rr_target_warnings(${target})
    _rr_apply_clang_tidy(${target})
    gtest_discover_tests(${target}
        DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS ${module}
        TEST_PREFIX "${module}.")
endfunction()
