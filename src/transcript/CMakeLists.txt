# rr_transcript — captions into the transcript: four formats read into timed
# words, a model's punctuation laid onto automatic captions without changing a
# word, and the words cut into numbered sentences with their times
# (docs/design/transcript.md). Pure functions. Hub only.
rr_add_module(transcript
    SOURCES
        captions.cpp
        cues.cpp
        json3.cpp
        projection.cpp
        sentences.cpp
        srt.cpp
        vtt.cpp
    PUBLIC_DEPS
        rr::core
        rr::knowledge
    PRIVATE_DEPS
        rr::json
        rr::text)
