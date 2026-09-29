# Sanitizer support (RR_SANITIZE).
#
# Sanitizers are applied *globally* (every target, including dependencies built
# from source) because ASan/TSan work best when all code in the process is
# instrumented; TSan in particular reports false races across uninstrumented
# code. Supported values:
#   "address;undefined"  AddressSanitizer + UndefinedBehaviorSanitizer (dev-clang preset)
#   "thread"             ThreadSanitizer (tsan preset)
#
# UBSan is configured to *abort* on the first report (-fno-sanitize-recover) so a
# test that triggers undefined behavior fails instead of printing and passing.
include_guard(GLOBAL)

if(RR_SANITIZE STREQUAL "")
    return()
endif()

if(MSVC)
    message(FATAL_ERROR "RR_SANITIZE is only supported with GCC/Clang in this project.")
endif()

list(JOIN RR_SANITIZE "," _rr_sanitize_csv)

if("thread" IN_LIST RR_SANITIZE AND ("address" IN_LIST RR_SANITIZE))
    message(FATAL_ERROR "ThreadSanitizer cannot be combined with AddressSanitizer.")
endif()

add_compile_options(
    -fsanitize=${_rr_sanitize_csv}
    -fno-omit-frame-pointer
    -fno-optimize-sibling-calls)
add_link_options(-fsanitize=${_rr_sanitize_csv})

if("undefined" IN_LIST RR_SANITIZE)
    add_compile_options(-fno-sanitize-recover=all)
    add_link_options(-fno-sanitize-recover=all)
endif()

message(STATUS "Sanitizers enabled: ${_rr_sanitize_csv}")
