# rr_reason — many answers, one decision: votes per item and waves of samples
# where they disagree, the medoid, boundary consensus, pairwise judgements in
# both orders, lexicographic selection; and the tiers (config/tiers/,
# compiled in) that say how much each stage spends (docs/design/reasoning.md).
# Pure functions: no I/O, no model, no threads. Hub only.
rr_add_embedded_library(tiers
    NAMESPACE rr::embedded::tiers
    FILES
        config/tiers/fast.toml
        config/tiers/max.toml
        config/tiers/thorough.toml)

rr_add_module(reason
    SOURCES
        boundaries.cpp
        pairwise.cpp
        select.cpp
        tier.cpp
        vote.cpp
    PUBLIC_DEPS
        rr::core
    PRIVATE_DEPS
        rr::embedded_tiers
        rr::json
        tomlplusplus::tomlplusplus)
