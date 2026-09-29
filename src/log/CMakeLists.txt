# rr_log — structured logging (console + rotating JSON Lines) with per-thread context.
rr_add_module(log
    SOURCES
        log.cpp
    PUBLIC_DEPS
        rr::core
    PRIVATE_DEPS
        nlohmann_json::nlohmann_json)
