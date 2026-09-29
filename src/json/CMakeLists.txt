# rr_json — JSON parsing, canonical serialization, schema validation and the
# portable-schema rules shared by constrained decoding and validation.
rr_add_module(json
    SOURCES
        json.cpp
        schema.cpp
    PUBLIC_DEPS
        rr::core
        nlohmann_json::nlohmann_json
    PRIVATE_DEPS
        jsoncons::jsoncons)
