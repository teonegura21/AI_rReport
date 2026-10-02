rr_add_test_suite(eval
    SOURCES
        annotate_stage_test.cpp
        annotate_test.cpp
        export_test.cpp
        gold_test.cpp
        judgement_test.cpp
        panel_test.cpp
        run_test.cpp
        score_test.cpp
        set_test.cpp
        sheet_test.cpp
        snapshot_test.cpp
    DEPS
        rr::eval
        rr::library
        rr::llm
        rr::oracle
        rr::prompt
        rr::report)

# The panel shipped (eval/judges/) is read from the source tree, as it is committed.
target_compile_definitions(rr_eval_tests PRIVATE RR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
