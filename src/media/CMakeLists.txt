# rr_media — where a video comes from: yt-dlp's description of it, the captions
# chosen in the language spoken (never a translation), and yt-dlp run safely,
# its failures told in rReport's terms (docs/design/transcript.md). Hub only.
rr_add_module(media
    SOURCES
        video.cpp
        ytdlp.cpp
    PUBLIC_DEPS
        rr::core
        rr::json
        rr::transcript
    PRIVATE_DEPS
        rr::proc)
