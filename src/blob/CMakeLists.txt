# rr_blob — content-addressed, crash-safe file storage (BLAKE3-named blobs).
# Shared: the hub stores media in it, the runtime receives uploads into it.
rr_add_module(blob
    SOURCES
        blob_store.cpp
    PUBLIC_DEPS
        rr::core)
