# rr_json — JSON parsing, canonical serialization, schema validation and the
# portable-schema rules shared by constrained decoding and validation; and
# where each value of a text was written (spans, P28).
rr_add_module(json
    SOURCES
        json.cpp
        schema.cpp
        spans.cpp
    PUBLIC_DEPS
        rr::core
        nlohmann_json::nlohmann_json
    PRIVATE_DEPS
        jsoncons::jsoncons)
