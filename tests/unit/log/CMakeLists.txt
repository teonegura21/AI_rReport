rr_add_test_suite(log
    SOURCES
        log_test.cpp
    DEPS
        rr::log
        nlohmann_json::nlohmann_json)
