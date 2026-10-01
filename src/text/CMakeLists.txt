# rr_text — text made comparable: Unicode normalization and folding, tokens,
# sentences, numbers by value (spoken ones too), names and mentions, quote
# alignment, and BM25 retrieval (docs/design/text.md). Every fidelity check
# rests on it. Pure functions: no I/O, no model, no threads. Hub only.
rr_add_module(text
    SOURCES
        align.cpp
        bm25.cpp
        entities.cpp
        lexicon.cpp
        numbers.cpp
        sentences.cpp
        tokens.cpp
        unicode.cpp
    PUBLIC_DEPS
        rr::core
    PRIVATE_DEPS
        utf8proc::utf8proc)
