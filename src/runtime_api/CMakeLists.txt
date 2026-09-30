# rr_runtime_api — the hub <-> runtime wire protocol: paths, the health report
# and its strict JSON form, validation of path parameters. Linked by both sides.
rr_add_module(runtime_api
    SOURCES
        protocol.cpp
    PUBLIC_DEPS
        rr::core
        rr::json)
