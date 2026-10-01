# rr_llm — calling models: one vocabulary for chat calls; the OpenAI-compatible
# streaming provider (llama-server behind the runtime); retries; accounting;
# record/replay; sampling presets and checks; answers validated against a
# schema. Hub only.
rr_add_module(llm
    SOURCES
        accounting.cpp
        chat.cpp
        openai_provider.cpp
        provider.cpp
        replay.cpp
        sampling.cpp
        structured.cpp
    PUBLIC_DEPS
        rr::core
        rr::json
        rr::net
    PRIVATE_DEPS
        rr::log)
