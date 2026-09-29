# Fail fast, with an explanation, when the compiler cannot build rReport.
#
# Why a feature probe and not only a version check: the real requirement is
# "std::expected is usable". Clang 18 *is* a C++23 compiler, but it reports
# __cpp_concepts == 201907L, and libstdc++ therefore hides <expected>. A plain
# version table would miss other combinations (e.g. AppleClang + libc++), so we
# compile a tiny probe and report what actually failed.
include_guard(GLOBAL)
include(CheckCXXSourceCompiles)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
    message(FATAL_ERROR
        "rReport needs GCC >= 13 (found ${CMAKE_CXX_COMPILER_VERSION}): "
        "std::expected and std::format are required (ADR-0007).")
endif()

set(CMAKE_REQUIRED_FLAGS "-std=c++23")
if(MSVC)
    set(CMAKE_REQUIRED_FLAGS "/std:c++latest")
endif()

check_cxx_source_compiles([[
    #include <expected>
    #include <format>
    #include <source_location>
    #include <stop_token>
    #include <string>
    std::expected<int, std::string> probe(int v) {
        if (v < 0) { return std::unexpected(std::format("{} < 0", v)); }
        return v;
    }
    int main() {
        std::stop_source source;
        return probe(1).value_or(0) == 1 && !source.stop_requested() ? 0 : 1;
    }
]] RR_HAS_CXX23_EXPECTED)

unset(CMAKE_REQUIRED_FLAGS)

if(NOT RR_HAS_CXX23_EXPECTED)
    message(FATAL_ERROR
        "The C++ compiler '${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}' cannot use "
        "std::expected/std::format/std::stop_token in C++23 mode.\n"
        "Supported: GCC >= 13, or Clang >= 19 (Clang 18 with libstdc++ hides <expected> because it "
        "reports __cpp_concepts < 202002L). On Ubuntu 24.04: `sudo apt install clang-20` and "
        "configure with -DCMAKE_CXX_COMPILER=clang++-20. See docs/adr/0007-errors-std-expected.md.")
endif()
