rr_add_test_suite(runtime_client
    SOURCES
        pod_manager_test.cpp
        runtime_client_test.cpp
    DEPS
        rr::fakes
        rr::runtime_client
        rr::supervisor)

# End to end: the client talks to a real Supervisor, whose engines are
# rr_fake_engine processes. Build it first, and tell the tests where it is.
target_compile_definitions(rr_runtime_client_tests PRIVATE RR_FAKE_ENGINE="$<TARGET_FILE:rr_fake_engine>")
add_dependencies(rr_runtime_client_tests rr_fake_engine)
