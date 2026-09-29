# rReport — root build.
#
# Two products share one source tree (ADR-0001):
#   * the hub      (apps/rreport)          — runs on the user's machine, owns all state;
#   * the runtime  (apps/rreport-runtime)  — the CUDA-free supervisor on the GPU pod.
#
# Every C++ module lives in src/<module>/ and is declared with rr_add_module()
# (cmake/RrModule.cmake), which enforces layering: a module can only include the
# public headers of the modules it links against.
cmake_minimum_required(VERSION 3.28)

project(rreport
    VERSION 0.1.0
    DESCRIPTION "Understand any video completely, then go beyond it"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Options (presets in CMakePresets.json set these for each workflow).
# ---------------------------------------------------------------------------
option(RR_BUILD_HUB "Build the hub (rreport) and hub-only modules" ON)
option(RR_BUILD_RUNTIME "Build the runtime supervisor (rreport-runtime)" ON)
option(RR_BUILD_TESTS "Build the unit tests" ON)
option(RR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(RR_FORCE_FETCH "Ignore system packages and build pinned dependency versions" OFF)
set(RR_SANITIZE "" CACHE STRING "Sanitizers to enable: 'address;undefined', 'thread', or empty")
set(RR_CLANG_TIDY "" CACHE STRING
    "clang-tidy command line applied to rReport targets only (never to dependencies)")

# ---------------------------------------------------------------------------
# Language settings. C++23 for std::expected (ADR-0007); no GNU extensions.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# We do not use C++20 modules; skipping the dependency scan speeds up builds.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(RrToolchainCheck)   # fail early, with an explanation, on unsupported compilers
include(RrSanitizers)       # global: must precede dependencies so they are instrumented too
include(RrWarnings)
include(RrModule)
include(RrEmbed)
include(RrDependencies)

if(RR_BUILD_TESTS)
    enable_testing()
    include(GoogleTest)
endif()

add_subdirectory(src)
add_subdirectory(apps)

if(RR_BUILD_TESTS)
    add_subdirectory(tests)
endif()

message(STATUS "rReport ${PROJECT_VERSION}: hub=${RR_BUILD_HUB} runtime=${RR_BUILD_RUNTIME} "
               "tests=${RR_BUILD_TESTS} sanitize='${RR_SANITIZE}' werror=${RR_WARNINGS_AS_ERRORS}")
