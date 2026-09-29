# rr_core — the vocabulary every other module speaks: Result/Error, IDs, time,
# hashing, secrets, randomness, threads and concurrency limits.

execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE RR_GIT_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _rr_git_status)
if(NOT _rr_git_status EQUAL 0 OR RR_GIT_REVISION STREQUAL "")
    set(RR_GIT_REVISION "unknown")
endif()
configure_file(version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/include/rr/core/version.hpp" @ONLY)

rr_add_module(core
    SOURCES
        assert.cpp
        error.cpp
        hash.cpp
        limiter.cpp
        random.cpp
        secret.cpp
        strings.cpp
        thread_pool.cpp
        time_span.cpp
        ulid.cpp
    PUBLIC_DEPS
        Threads::Threads
    PRIVATE_DEPS
        BLAKE3::blake3
        OpenSSL::Crypto)

target_include_directories(rr_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>)
