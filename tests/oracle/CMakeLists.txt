# rr::oracle — a model that answers every mission right from the request
# alone, and the faults real models make, on the calls a test picks
# (rr/oracle/oracle.hpp, docs/design/testing.md). Test support: never linked
# into a product.
rr_add_module(oracle
    SOURCES
        answers.cpp
        oracle.cpp
    PUBLIC_DEPS
        rr::core
        rr::llm)
