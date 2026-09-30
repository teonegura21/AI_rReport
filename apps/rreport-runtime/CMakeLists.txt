# rreport-runtime — the GPU pod's supervisor (ADR-0001, ADR-0003). Thin: it
# parses arguments and wires rr_supervisor together (see main.cpp).
rr_add_executable(rreport-runtime
    SOURCES
        main.cpp
    DEPS
        rr::supervisor
        rr::config
        rr::log
        rr::net
        rr::proc
        CLI11::CLI11)
