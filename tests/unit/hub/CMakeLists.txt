rr_add_test_suite(hub
    SOURCES
        doctor_test.cpp
        hub_config_test.cpp
    DEPS
        rr::fakes
        rr::hub)
