# rr_report — a job's article as one web page: the article retold, each
# sentence linked to the moment it tells, and the Fidelity Report (what was
# kept, what was left out and why), rendered from the artifacts whenever
# asked, as the library holds them (docs/design/report.md). Hub only.
rr_add_module(report
    SOURCES
        contents.cpp
        page.cpp
    PUBLIC_DEPS
        rr::core
        rr::fidelity
        rr::knowledge
        rr::library
        rr::media
    PRIVATE_DEPS
        rr::json
        rr::pipeline)
