# Component tests: the real executables, driven from the outside.
if(RR_BUILD_RUNTIME)
    rr_add_test_suite(runtime_app
        SOURCES
            runtime_app_test.cpp
        DEPS
            rr::core
            rr::fakes
            rr::net
            rr::proc)
    # The binary under test, and the llama-server stand-in it runs.
    target_compile_definitions(rr_runtime_app_tests PRIVATE
        RR_RUNTIME_BINARY="$<TARGET_FILE:rreport-runtime>"
        RR_FAKE_ENGINE="$<TARGET_FILE:rr_fake_engine>")
    add_dependencies(rr_runtime_app_tests rreport-runtime rr_fake_engine)
endif()

# The hub's executable, against a real runtime and a fake RunPod API.
if(RR_BUILD_HUB AND RR_BUILD_RUNTIME)
    rr_add_test_suite(hub_cli
        SOURCES
            hub_cli_test.cpp
        DEPS
            rr::core
            rr::fakes
            rr::json
            rr::library
            rr::net
            rr::proc)
    target_compile_definitions(rr_hub_cli_tests PRIVATE
        RR_HUB_BINARY="$<TARGET_FILE:rreport>"
        RR_RUNTIME_BINARY="$<TARGET_FILE:rreport-runtime>"
        RR_FAKE_ENGINE="$<TARGET_FILE:rr_fake_engine>")
    add_dependencies(rr_hub_cli_tests rreport rreport-runtime rr_fake_engine)
endif()

# The intelligence core end to end: the hub's executable through every stage,
# against a real runtime whose fake engines answer as the oracle; a job killed
# and resumed (plan P22).
if(RR_BUILD_HUB AND RR_BUILD_RUNTIME)
    rr_add_test_suite(end_to_end
        SOURCES
            end_to_end_test.cpp
        DEPS
            rr::core
            rr::db
            rr::eval
            rr::json
            rr::library
            rr::net
            rr::oracle
            rr::pipeline
            rr::proc)
    target_compile_definitions(rr_end_to_end_tests PRIVATE
        RR_HUB_BINARY="$<TARGET_FILE:rreport>"
        RR_RUNTIME_BINARY="$<TARGET_FILE:rreport-runtime>"
        RR_FAKE_ENGINE="$<TARGET_FILE:rr_fake_engine>")
    add_dependencies(rr_end_to_end_tests rreport rreport-runtime rr_fake_engine)
endif()
