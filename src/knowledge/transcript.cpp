#include <rr/knowledge/transcript.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

#include <rr/knowledge/artifacts.hpp>

#include "fields.hpp"

namespace rr::knowledge {

const Sentence* Transcript::find(SentenceId id) const noexcept {
    // S1…Sn sit at their number's place in a transcript that was read; one
    // built otherwise is searched.
    const std::size_t index = id.number() - 1;
    if (index < sentences.size() && sentences[index].id == id) {
        return &sentences[index];
    }
    const auto found = std::ranges::find(sentences, id, &Sentence::id);
    return found == sentences.end() ? nullptr : &*found;
}

json::Json to_json(const Transcript& transcript) {
    json::Json sentences = json::Json::array();
    for (const Sentence& sentence : transcript.sentences) {
        sentences.push_back(json::Json{{"id", sentence.id.str()},
                                       {"text", sentence.text},
                                       {"start_ms", sentence.span.start().count()},
                                       {"end_ms", sentence.span.end().count()},
                                       {"speaker", fields::json_or_null(sentence.speaker)}});
    }
    return json::Json{{"sentences", std::move(sentences)}};
}

Result<Transcript> transcript_from_json(const json::Json& document) {
    RR_TRY(artifact_schemas().validate("artifacts/transcript", document));
    Transcript transcript;
    for (const json::Json& item : document.at("sentences")) {
        const std::string written = item.at("id").get<std::string>();
        const SentenceId expected{static_cast<std::uint32_t>(transcript.sentences.size() + 1)};
        if (SentenceId::parse(written) != expected) {
            return fail(
                Errc::invalid_argument,
                std::format(
                    "sentence {} is out of place: sentences run S1, S2, \u2026 in order, and {} comes next",
                    written, expected));
        }
        RR_TRY_ASSIGN(const std::int64_t start, fields::count(item.at("start_ms"), "start_ms"));
        RR_TRY_ASSIGN(const std::int64_t end, fields::count(item.at("end_ms"), "end_ms"));
        const Result<TimeSpan> span = TimeSpan::make(Millis{start}, Millis{end});
        if (!span.has_value()) {
            return fail(Errc::invalid_argument, std::format("{} ends before it starts", written));
        }
        if (!transcript.sentences.empty() && span->start() < transcript.sentences.back().span.start()) {
            return fail(Errc::invalid_argument,
                        std::format("{} starts before {}: sentences are in the order said", written,
                                    transcript.sentences.back().id));
        }
        transcript.sentences.push_back(Sentence{.id = expected,
                                                .text = item.at("text").get<std::string>(),
                                                .span = *span,
                                                .speaker = fields::optional_string(item.at("speaker"))});
    }
    return transcript;
}

}  // namespace rr::knowledge
