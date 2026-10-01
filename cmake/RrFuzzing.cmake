# Fuzzing support (RR_FUZZ).
#
# A fuzz target is a function, LLVMFuzzerTestOneInput(), that checks invariants
# on whatever bytes it is given and aborts when one breaks. Each target is
# built two ways (rr_add_fuzz_target(), tests/fuzz/CMakeLists.txt):
#   * always, with the tests: a replay of its committed corpus, run by CTest
#     under every compiler and sanitizer;
#   * with RR_FUZZ=ON: linked with libFuzzer, which searches for inputs that
#     break the invariants, guided by the code coverage they reach.
#
# RR_FUZZ instruments every target for coverage (-fsanitize=fuzzer-no-link), so
# the fuzzer sees inside the libraries it calls, not only the target. It needs
# Clang, whose runtime libFuzzer is part of, and a sanitizer, whose runtime
# holds the coverage hooks the instrumented code calls: the `fuzz` preset adds
# AddressSanitizer and UBSan, which also turn silent memory errors into crashes
# the fuzzer can see.
include_guard(GLOBAL)

if(NOT RR_FUZZ)
    return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "RR_FUZZ needs Clang: libFuzzer ships with its compiler runtime.")
endif()
if(RR_SANITIZE STREQUAL "")
    message(FATAL_ERROR "RR_FUZZ needs a sanitizer (RR_SANITIZE): its runtime holds the coverage hooks.")
endif()

add_compile_options(-fsanitize=fuzzer-no-link)
message(STATUS "Fuzzing enabled: coverage instrumentation for libFuzzer")
