# rreport — the hub (ADR-0001). Thin: it parses arguments and wires rr_hub,
# rr_pipeline, rr_runtime_client and rr_library together (see main.cpp).
rr_add_executable(rreport
    SOURCES
        main.cpp
    DEPS
        rr::hub
        rr::library
        rr::llm
        rr::media
        rr::pipeline
        rr::prompt
        rr::reason
        rr::runpod
        rr::runtime_client
        rr::config
        rr::log
        rr::net
        rr::proc
        CLI11::CLI11)
