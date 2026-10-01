# Live tests (L4): the runtime against a real llama-server, which the nightly
# job builds for the CPU (.github/workflows/nightly.yml). Built and run
# everywhere, so they cannot rot; each test skips itself unless
# RR_LIVE_LLAMA_SERVER and RR_LIVE_MODEL name the server and the model.
if(RR_BUILD_HUB AND RR_BUILD_RUNTIME)
    rr_add_test_suite(live
        SOURCES
            live_test.cpp
        DEPS
            rr::core
            rr::json
            rr::llm
            rr::net
            rr::proc
            rr::runtime_client)
    target_compile_definitions(rr_live_tests PRIVATE RR_RUNTIME_BINARY="$<TARGET_FILE:rreport-runtime>")
    add_dependencies(rr_live_tests rreport-runtime)
endif()
