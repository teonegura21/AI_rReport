# rr_fake_engine — a scriptable stand-in for llama-server, used by the
# supervisor's end-to-end tests (see main.cpp for the flags it understands).
rr_add_executable(rr_fake_engine
    SOURCES
        main.cpp
    DEPS
        rr::net
        rr::json)
