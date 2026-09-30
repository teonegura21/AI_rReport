rr_add_test_suite(supervisor
    SOURCES
        catalog_test.cpp
        gpu_test.cpp
        models_test.cpp
        runtime_config_test.cpp
        supervisor_test.cpp
        watchdog_test.cpp
    DEPS
        rr::supervisor
        rr::fake_engine_stream
        rr::fakes)

# The end-to-end tests run real engine processes: rr_fake_engine stands in for
# llama-server. Build it first, and tell the tests where it is.
target_compile_definitions(rr_supervisor_tests PRIVATE RR_FAKE_ENGINE="$<TARGET_FILE:rr_fake_engine>")
# And where the sources are: one test reads the catalog the image ships.
target_compile_definitions(rr_supervisor_tests PRIVATE RR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_dependencies(rr_supervisor_tests rr_fake_engine)
