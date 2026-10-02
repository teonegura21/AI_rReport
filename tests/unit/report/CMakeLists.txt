rr_add_test_suite(report
    SOURCES
        contents_test.cpp
        page_test.cpp
    DEPS
        rr::oracle
        rr::pipeline
        rr::report)
