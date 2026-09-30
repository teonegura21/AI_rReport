# rr_fake_engine — a scriptable stand-in for llama-server, for end-to-end tests
# (see main.cpp for the flags and knobs it understands).
#
# rr::fake_engine_stream (header-only) says what it streams, llama-server's
# way; the tests that check what arrives use it too.
rr_add_module(fake_engine_stream
    PUBLIC_DEPS
        rr::json)

rr_add_executable(rr_fake_engine
    SOURCES
        main.cpp
    DEPS
        rr::fake_engine_stream
        rr::net
        rr::json)
