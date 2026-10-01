# Fuzz targets (cmake/RrFuzzing.cmake, docs/design/testing.md).
#
# rr_add_fuzz_target(<name> SOURCES <files...> [DEPS <targets...>] CORPUS <dir>)
#   <files> define LLVMFuzzerTestOneInput(), which aborts on a broken invariant.
#     * `rr_<name>_fuzz_replay` runs it on every file of <dir> (replay_main.cpp),
#       registered with CTest as `fuzz.<name>` (labels `fuzz` and <name>);
#     * with RR_FUZZ=ON, `rr_<name>_fuzzer` is the libFuzzer binary:
#         rr_<name>_fuzzer -max_total_time=600 new-inputs/ tests/fuzz/corpus/<name>
#       Inputs it finds worth keeping (a crash, after its fix; a new path)
#       belong in the corpus, so every push replays them.
function(rr_add_fuzz_target name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "CORPUS" "SOURCES;DEPS")
    if(ARG_UNPARSED_ARGUMENTS OR NOT ARG_SOURCES)
        message(FATAL_ERROR "rr_add_fuzz_target(${name}): SOURCES are required; unknown: ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT IS_DIRECTORY "${ARG_CORPUS}")
        message(FATAL_ERROR "rr_add_fuzz_target(${name}): CORPUS must be a directory, not '${ARG_CORPUS}'")
    endif()

    set(replay rr_${name}_fuzz_replay)
    add_executable(${replay} ${ARG_SOURCES} replay_main.cpp)
    target_link_libraries(${replay} PRIVATE ${ARG_DEPS})
    target_compile_features(${replay} PRIVATE cxx_std_23)
    rr_target_warnings(${replay})
    _rr_apply_clang_tidy(${replay})
    add_test(NAME fuzz.${name} COMMAND ${replay} "${ARG_CORPUS}")
    set_tests_properties(fuzz.${name} PROPERTIES LABELS "fuzz;${name}")

    if(RR_FUZZ)
        set(fuzzer rr_${name}_fuzzer)
        add_executable(${fuzzer} ${ARG_SOURCES})
        target_link_libraries(${fuzzer} PRIVATE ${ARG_DEPS})
        target_compile_features(${fuzzer} PRIVATE cxx_std_23)
        target_link_options(${fuzzer} PRIVATE -fsanitize=fuzzer)
        rr_target_warnings(${fuzzer})
    endif()
endfunction()

# Text from anywhere: captions, model output, a file of unknown encoding.
rr_add_fuzz_target(text
    SOURCES text_fuzz.cpp
    DEPS rr::text
    CORPUS ${CMAKE_CURRENT_SOURCE_DIR}/corpus/text)

# The article's markers, as a writer model emits them (ADR-0006).
rr_add_fuzz_target(markers
    SOURCES markers_fuzz.cpp
    DEPS rr::knowledge
    CORPUS ${CMAKE_CURRENT_SOURCE_DIR}/corpus/markers)

# Caption files, from YouTube or from disk, read into words, cut into
# sentences, and a model's punctuation projected onto them (rr_transcript).
rr_add_fuzz_target(captions
    SOURCES captions_fuzz.cpp
    DEPS rr::transcript
    CORPUS ${CMAKE_CURRENT_SOURCE_DIR}/corpus/captions)
