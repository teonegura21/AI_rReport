# rr_proc — running external programs: posix_spawn in a private process group,
# escalating stops, drained and capped output (ADR-0013). Shared by the hub
# (yt-dlp, ffmpeg) and the runtime (llama-server and other engines); and
# termination signals turned into a stop request, for the executables.
rr_add_module(proc
    SOURCES
        line_splitter.cpp
        process.cpp
        termination_signals.cpp
    PUBLIC_DEPS
        rr::core)
