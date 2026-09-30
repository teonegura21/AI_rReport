# rr_supervisor — the CUDA-free runtime on the GPU pod (ADR-0001, ADR-0003):
# GPU detection, the model catalog and GPU profiles, engine processes
# (llama-server), the streaming proxy, blobs, auth and the idle-stop watchdog.
rr_add_module(supervisor
    SOURCES
        catalog.cpp
        engine.cpp
        gpu.cpp
        proxy.cpp
        supervisor.cpp
        watchdog.cpp
    PUBLIC_DEPS
        rr::core
        rr::runtime_api
        rr::net
        rr::blob
        rr::proc
    PRIVATE_DEPS
        rr::json
        rr::log
        tomlplusplus::tomlplusplus)
