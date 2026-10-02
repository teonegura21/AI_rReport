# rreport — the hub (ADR-0001). Thin: it parses arguments and wires rr_hub,
# rr_pipeline, rr_report, rr_eval, rr_runtime_client and rr_library together
# (main.cpp; the eval's commands in eval_commands.cpp; what both share in
# cli.hpp).
rr_add_executable(rreport
    SOURCES
        cli.cpp
        eval_commands.cpp
        main.cpp
    DEPS
        rr::eval
        rr::hub
        rr::library
        rr::llm
        rr::media
        rr::pipeline
        rr::prompt
        rr::reason
        rr::report
        rr::runpod
        rr::runtime_client
        rr::config
        rr::log
        rr::net
        rr::proc
        CLI11::CLI11)
