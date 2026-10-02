rr_add_test_suite(eval
    SOURCES
        annotate_stage_test.cpp
        annotate_test.cpp
        calibrate_test.cpp
        calibration_test.cpp
        compare_test.cpp
        export_test.cpp
        flow_test.cpp
        gate_test.cpp
        gold_test.cpp
        history_test.cpp
        judgement_test.cpp
        labels_test.cpp
        metrics_test.cpp
        panel_test.cpp
        run_test.cpp
        score_test.cpp
        set_test.cpp
        sheet_test.cpp
        snapshot_test.cpp
        stats_test.cpp
        sweep_test.cpp
    DEPS
        rr::eval
        rr::library
        rr::llm
        rr::oracle
        rr::prompt
        rr::report)

# The panel and the gate shipped (eval/judges/, eval/gates/) are read from the
# source tree, as they are committed.
target_compile_definitions(rr_eval_tests PRIVATE RR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
