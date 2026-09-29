# rr_db — SQLite without exceptions: scoped transactions, a WAL-friendly
# writer + reader-pool design, and ordered migrations (ADR-0008).
rr_add_module(db
    SOURCES
        database.cpp
    PUBLIC_DEPS
        rr::core
    PRIVATE_DEPS
        SQLite::SQLite3)
