# rr_proc — running external programs: posix_spawn in a private process group,
# escalating stops, drained and capped output (ADR-0013). Shared by the hub
# (yt-dlp, ffmpeg) and the runtime (llama-server and other engines).
rr_add_module(proc
    SOURCES
        line_splitter.cpp
        process.cpp
    PUBLIC_DEPS
        rr::core)
