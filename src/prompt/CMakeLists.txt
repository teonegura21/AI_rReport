# rr_prompt — the prompts: their templates, their manifest and numbered
# versions, cache-aware rendering, and token estimates
# (docs/design/prompts.md).

# The prompts, and the schemas they name, are compiled in. The glob runs at
# configure time; CONFIGURE_DEPENDS re-runs it when a file is added.
file(GLOB_RECURSE _rr_prompt_files CONFIGURE_DEPENDS RELATIVE "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/prompts/*" "${PROJECT_SOURCE_DIR}/schemas/*")
list(SORT _rr_prompt_files)
rr_add_embedded_library(prompts
    NAMESPACE rr::embedded::prompts
    FILES ${_rr_prompt_files})

rr_add_module(prompt
    SOURCES
        library.cpp
        tokens.cpp
    PUBLIC_DEPS
        rr::core
        rr::json
    PRIVATE_DEPS
        rr::embedded_prompts
        pantor::inja
        tomlplusplus::tomlplusplus)
