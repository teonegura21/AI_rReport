# Component tests: the real executables, driven from the outside.
if(RR_BUILD_RUNTIME)
    rr_add_test_suite(runtime_app
        SOURCES
            runtime_app_test.cpp
        DEPS
            rr::core
            rr::net
            rr::proc)
    # The binary under test, and the llama-server stand-in it runs.
    target_compile_definitions(rr_runtime_app_tests PRIVATE
        RR_RUNTIME_BINARY="$<TARGET_FILE:rreport-runtime>"
        RR_FAKE_ENGINE="$<TARGET_FILE:rr_fake_engine>")
    add_dependencies(rr_runtime_app_tests rreport-runtime rr_fake_engine)
endif()
