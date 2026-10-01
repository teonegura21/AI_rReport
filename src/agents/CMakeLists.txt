# rr_agents — the army (ADR-0011): missions run on the pod's engines, as many
# at once as each engine holds, escalated when their answers fail, within the
# job's ceilings (ADR-0018), and logged (docs/design/agents.md). Hub only.
rr_add_module(agents
    SOURCES
        army.cpp
        ladder.cpp
        mission.cpp
        sizing.cpp
    PUBLIC_DEPS
        rr::core
        rr::json
        rr::llm
        rr::prompt
    PRIVATE_DEPS
        rr::log)
