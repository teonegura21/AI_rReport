# Third-party dependencies — the single place where versions are pinned.
#
# Strategy (ADR-0012): every library is declared with FetchContent's
# FIND_PACKAGE_ARGS. CMake first calls find_package() with a minimum version,
# which picks up system packages (apt), a vcpkg toolchain or Conan. Only if that
# fails does it clone the pinned git tag and build it in-tree.
#
# Large C libraries (libcurl, OpenSSL, SQLite) are system-only: building them
# from source would dominate build times and add risk for no benefit.
#
# -DRR_FORCE_FETCH=ON ignores system packages entirely (CI uses it to prove the
# fallback path still works).
include_guard(GLOBAL)
include(FetchContent)

set(FETCHCONTENT_QUIET ON)
if(RR_FORCE_FETCH)
    set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)
else()
    set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE OPT_IN)
endif()

# ---------------------------------------------------------------------------
# System-only dependencies.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)   # SHA-256 for model verification
find_package(CURL 7.85 REQUIRED)                       # HTTP client (rr_net); 7.85: CURLOPT_PROTOCOLS_STR
find_package(SQLite3 3.38 REQUIRED)                    # library database (rr_db); FTS5 checked at runtime

# ---------------------------------------------------------------------------
# System first, pinned git tag as fallback.
# ---------------------------------------------------------------------------
# JSON for everything that crosses a process boundary.
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
    SYSTEM
    FIND_PACKAGE_ARGS 3.11 CONFIG)

# TOML parser for layered configuration (rr_config).
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE
    SYSTEM
    FIND_PACKAGE_ARGS 3.4 CONFIG)

# Command-line parsing for the apps.
FetchContent_Declare(CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.4.2
    GIT_SHALLOW TRUE
    SYSTEM
    FIND_PACKAGE_ARGS 2.4 CONFIG)

# HTTP server (runtime supervisor, hub API) — header-only. The apt version
# (0.14) predates important fixes, so the minimum forces a recent release.
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)        # TLS is terminated in front of us
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.58.0
    GIT_SHALLOW TRUE
    SYSTEM
    FIND_PACKAGE_ARGS 0.20 CONFIG)

# JSON Schema (draft 2020-12) validation, used behind rr_json's pimpl.
set(JSONCONS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(jsoncons
    GIT_REPOSITORY https://github.com/danielaparker/jsoncons.git
    GIT_TAG v1.9.0
    GIT_SHALLOW TRUE
    SYSTEM
    FIND_PACKAGE_ARGS 1.3 CONFIG)

# BLAKE3: fast cryptographic content hashing (cache keys, blob addresses).
FetchContent_Declare(BLAKE3
    GIT_REPOSITORY https://github.com/BLAKE3-team/BLAKE3.git
    GIT_TAG 1.8.7
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR c
    SYSTEM
    FIND_PACKAGE_ARGS 1.5 CONFIG)

# Prompt templates (rr_prompt): Jinja's syntax in C++, header-only. Its own
# CMake is not run (it executes a script at configure time and sets up
# installs); its headers are all rReport needs, given a target below.
FetchContent_Declare(inja
    GIT_REPOSITORY https://github.com/pantor/inja.git
    GIT_TAG v3.5.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR headers-only  # no such directory: populate, do not add_subdirectory()
    FIND_PACKAGE_ARGS 3.4 CONFIG)

set(_rr_dependencies nlohmann_json tomlplusplus CLI11 httplib jsoncons BLAKE3 inja)

# Unicode (rr_text): normalization, case folding, character classes. A small C
# library whose system package has no CMake config, only pkg-config: the
# system's through pkg-config, else the pinned tag built in-tree.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT RR_FORCE_FETCH)
    pkg_check_modules(UTF8PROC QUIET IMPORTED_TARGET GLOBAL libutf8proc)
endif()
if(NOT TARGET PkgConfig::UTF8PROC)
    set(UTF8PROC_INSTALL OFF CACHE BOOL "" FORCE)
    set(UTF8PROC_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(utf8proc
        GIT_REPOSITORY https://github.com/JuliaStrings/utf8proc.git
        GIT_TAG v2.9.0
        GIT_SHALLOW TRUE
        SYSTEM)
    list(APPEND _rr_dependencies utf8proc)
endif()

if(RR_BUILD_TESTS)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(GTest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE
        SYSTEM
        FIND_PACKAGE_ARGS 1.14 NAMES GTest CONFIG)
    list(APPEND _rr_dependencies GTest)
endif()

FetchContent_MakeAvailable(${_rr_dependencies})

# Normalize target names: some projects only define namespaced aliases when
# installed, not when built in-tree. Everything below links the namespaced name.
if(TARGET jsoncons AND NOT TARGET jsoncons::jsoncons)
    add_library(jsoncons::jsoncons ALIAS jsoncons)
endif()
if(TARGET blake3 AND NOT TARGET BLAKE3::blake3)
    add_library(BLAKE3::blake3 ALIAS blake3)
endif()
if(TARGET PkgConfig::UTF8PROC)
    add_library(utf8proc::utf8proc ALIAS PkgConfig::UTF8PROC)
elseif(TARGET utf8proc AND NOT TARGET utf8proc::utf8proc)
    add_library(utf8proc::utf8proc ALIAS utf8proc)
endif()
if(NOT TARGET pantor::inja)  # fetched: headers only, against our nlohmann_json
    add_library(rr_inja INTERFACE)
    target_include_directories(rr_inja SYSTEM INTERFACE "${inja_SOURCE_DIR}/include")
    target_link_libraries(rr_inja INTERFACE nlohmann_json::nlohmann_json)
    add_library(pantor::inja ALIAS rr_inja)
endif()

foreach(_rr_required_target IN ITEMS
        nlohmann_json::nlohmann_json tomlplusplus::tomlplusplus CLI11::CLI11
        httplib::httplib jsoncons::jsoncons BLAKE3::blake3 pantor::inja utf8proc::utf8proc
        OpenSSL::Crypto CURL::libcurl SQLite::SQLite3)
    if(NOT TARGET ${_rr_required_target})
        message(FATAL_ERROR "Dependency target ${_rr_required_target} is missing after resolution")
    endif()
endforeach()
