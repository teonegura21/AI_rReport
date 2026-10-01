rr_add_test_suite(reason
    SOURCES
        boundaries_test.cpp
        pairwise_test.cpp
        select_test.cpp
        tier_test.cpp
        vote_test.cpp
    DEPS
        rr::reason)

# The tiers compiled in are compared with the source tree's.
target_compile_definitions(rr_reason_tests PRIVATE RR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
