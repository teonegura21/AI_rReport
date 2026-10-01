// rr/knowledge/transcript.hpp — what was said, sentence by sentence.
//
// Why this exists
//   The transcript is the closed world everything else answers to (ADR-0005).
//   Its sentences are numbered S1…Sn, each with when it was said and, when
//   known, who said it. Atoms name the sentences they come from, every
//   sentence must be accounted for (Ledger A), and through them each sentence
//   of the article reaches its second of the video. The transcript stage
//   writes it (plan P14).
//
// Thread-safety: plain values.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/core/time_span.hpp>
#include <rr/json/json.hpp>
#include <rr/knowledge/ids.hpp>

namespace rr::knowledge {

struct Sentence {
    SentenceId id;
    std::string text;                      // as kept: NFC (rr/text/unicode.hpp)
    TimeSpan span;                         // when it was said
    std::optional<std::string> speaker{};  // who said it, when known; never guessed

    friend bool operator==(const Sentence&, const Sentence&) = default;
};

struct Transcript {
    std::vector<Sentence> sentences{};  // S1, S2, … Sn, in the order said

    // The sentence `id`, or nullptr.
    [[nodiscard]] const Sentence* find(SentenceId id) const noexcept;

    friend bool operator==(const Transcript&, const Transcript&) = default;
};

[[nodiscard]] json::Json to_json(const Transcript& transcript);

// Errc::invalid_argument when `document` breaks the schema
// (schemas/artifacts/transcript.json), when its sentences are not S1, S2, …
// in order, or when one ends before it starts or starts before the one
// before it.
[[nodiscard]] Result<Transcript> transcript_from_json(const json::Json& document);

}  // namespace rr::knowledge
