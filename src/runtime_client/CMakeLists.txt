# rr_runtime_client — the hub's side of the pod: the runtime protocol
# (health, heartbeats, blobs, readiness), the model provider aimed at the
# runtime, and PodManager, which starts and stops the pod through RunPod's
# API. Hub only.
rr_add_module(runtime_client
    SOURCES
        pod_manager.cpp
        runtime_client.cpp
    PUBLIC_DEPS
        rr::core
        rr::llm
        rr::net
        rr::runpod
        rr::runtime_api
    PRIVATE_DEPS
        rr::json
        rr::log)
