# rr_supervisor — the CUDA-free runtime on the GPU pod (ADR-0001, ADR-0003):
# GPU detection, the model catalog and GPU profiles, engine processes
# (llama-server), model files (verified, resumably downloaded), the streaming
# proxy, blobs, auth and the idle-stop watchdog; and the runtime's configuration.

# The runtime's built-in defaults, compiled in (they are also the schema).
rr_add_embedded_library(runtime_defaults
    NAMESPACE rr::embedded::runtime_defaults
    FILES config/runtime.default.toml)

rr_add_module(supervisor
    SOURCES
        catalog.cpp
        engine.cpp
        gpu.cpp
        models.cpp
        proxy.cpp
        runtime_config.cpp
        supervisor.cpp
        watchdog.cpp
    PUBLIC_DEPS
        rr::core
        rr::config
        rr::runtime_api
        rr::net
        rr::blob
        rr::proc
    PRIVATE_DEPS
        rr::embedded_runtime_defaults
        rr::json
        rr::runpod
        rr::log
        tomlplusplus::tomlplusplus)
