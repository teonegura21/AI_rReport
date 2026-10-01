rr_add_test_suite(prompt
    SOURCES
        library_test.cpp
        tokens_test.cpp
    DEPS
        rr::prompt)

# The L0 check compares the embedded prompts with the source tree's.
target_compile_definitions(rr_prompt_tests PRIVATE RR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
