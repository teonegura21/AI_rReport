# rr_runpod — RunPod's pod API (REST v2, and GraphQL's podStop for a pod
# stopping itself). Shared: the hub starts and stops the pod; the runtime
# stops it when idle.
rr_add_module(runpod
    SOURCES
        runpod.cpp
    PUBLIC_DEPS
        rr::core
        rr::net
    PRIVATE_DEPS
        rr::json
        rr::log)
