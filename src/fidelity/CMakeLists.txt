# rr_fidelity — the deterministic checks of an analysis: Ledger A (the atoms
# against the transcript) after atomize, Ledger B (the article against the
# atoms) after write. Every finding carries a repair message; the coverage
# counts are M1's acceptance measures (docs/design/fidelity.md). Hub only.
rr_add_module(fidelity
    SOURCES
        article.cpp
        finding.cpp
        held.cpp
        ledger.cpp
    PUBLIC_DEPS
        rr::core
        rr::knowledge
        rr::text)
