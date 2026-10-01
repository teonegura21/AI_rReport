# rr_hub — the hub's configuration (config/default.toml, compiled in); the
# stack every model call goes through (ModelStack: the runtime, and nowhere
# else); the army's engines, from the pod's health report and the sampling
# presets; and its operator tools: `rreport doctor` (what works, what does
# not, what to do) and `rreport bench` (what a GPU pod does per second and per
# dollar). The executable only wires them together. Hub only.
rr_add_embedded_library(hub_defaults
    NAMESPACE rr::embedded::hub_defaults
    FILES config/default.toml)

rr_add_module(hub
    SOURCES
        army_engines.cpp
        bench.cpp
        doctor.cpp
        hub_config.cpp
        model_stack.cpp
    PUBLIC_DEPS
        rr::agents
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
