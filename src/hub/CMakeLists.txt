# rr_hub — the hub's configuration (config/default.toml, compiled in) and its
# operator tools: `rreport doctor` (what works, what does not, what to do) and
# `rreport bench` (what a GPU pod does per second and per dollar). The
# executable only wires them together. Hub only.
rr_add_embedded_library(hub_defaults
    NAMESPACE rr::embedded::hub_defaults
    FILES config/default.toml)

rr_add_module(hub
    SOURCES
        bench.cpp
        doctor.cpp
        hub_config.cpp
    PUBLIC_DEPS
        rr::config
        rr::core
        rr::json
        rr::llm
        rr::net
        rr::runtime_api
        rr::runtime_client
    PRIVATE_DEPS
        rr::embedded_hub_defaults
        rr::library
        rr::log
        rr::runpod)
