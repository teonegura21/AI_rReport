// A transcript cut into windows its engines hold (rr/pipeline/windows.hpp).
#include <rr/pipeline/windows.hpp>

#include <format>
#include <utility>

#include <rr/core/assert.hpp>
#include <rr/core/time_span.hpp>

namespace rr::pipeline {

namespace {

// The bytes sentence `index` takes in a prompt: its line, and its newline.
std::size_t line_bytes(const knowledge::Transcript& transcript, std::size_t index) {
    return sentence_lines(transcript, index, index + 1).size() + 1;
}

}  // namespace

std::string sentence_lines(const knowledge::Transcript& transcript, std::size_t begin, std::size_t end) {
    std::string lines;
    for (std::size_t index = begin; index < end && index < transcript.sentences.size(); ++index) {
        const knowledge::Sentence& sentence = transcript.sentences[index];
        lines += std::format("{}{} [{}] {}", lines.empty() ? "" : "\n", sentence.id.str(),
                             format_timestamp(sentence.span.start()), sentence.text);
    }
    return lines;
}

std::string sentence_range(std::size_t begin, std::size_t end) {
    RR_ASSERT(begin < end, "a range names a sentence at least");
    return begin + 1 == end ? std::format("S{}", begin + 1) : std::format("S{}\u2013S{}", begin + 1, end);
}

std::string sentences_have(const knowledge::Transcript& transcript, std::size_t begin, std::size_t end) {
    const bool of_the_transcript = begin < end && end <= transcript.sentences.size();
    RR_ASSERT(of_the_transcript, "a finding is about some of the transcript's sentences");
    const bool one = begin + 1 == end;
    return std::format("the {} {} ({} to {}) {}", one ? "sentence" : "sentences", sentence_range(begin, end),
                       format_timestamp(transcript.sentences[begin].span.start()),
                       format_timestamp(transcript.sentences[end - 1].span.end()), one ? "has" : "have");
}

std::size_t lines_bytes(const knowledge::Transcript& transcript, const SentenceWindow& window) {
    std::size_t bytes = 0;
    for (std::size_t index = window.begin; index < window.end; ++index) {
        bytes += line_bytes(transcript, index);
    }
    return bytes;
}

SentenceWindow window_of(const knowledge::Transcript& transcript, std::size_t begin, std::size_t end,
                         std::size_t context) {
    const bool of_the_transcript = begin <= end && end <= transcript.sentences.size();
    RR_ASSERT(of_the_transcript, "a window is of the transcript's sentences");
    SentenceWindow window{.begin = begin, .end = end, .context_begin = begin, .context_end = end};
    std::size_t used = 0;
    while (window.context_begin > 0 && begin - window.context_begin < context &&
           used + line_bytes(transcript, window.context_begin - 1) <= kContextBytes) {
        used += line_bytes(transcript, --window.context_begin);
    }
    used = 0;
    while (window.context_end < transcript.sentences.size() && window.context_end - end < context &&
           used + line_bytes(transcript, window.context_end) <= kContextBytes) {
        used += line_bytes(transcript, window.context_end++);
    }
    return window;
}

SentenceWindow window_from(const knowledge::Transcript& transcript, std::size_t begin, std::size_t bytes,
                           std::size_t context) {
    RR_ASSERT(begin < transcript.sentences.size(), "a window starts at one of the transcript's sentences");
    std::size_t end = begin + 1;
    std::size_t used = line_bytes(transcript, begin);
    while (end < transcript.sentences.size() && used + line_bytes(transcript, end) <= bytes) {
        used += line_bytes(transcript, end++);
    }
    return window_of(transcript, begin, end, context);
}

Result<WindowPlan> plan_windows(const knowledge::Transcript& transcript, std::size_t bytes,
                                const agents::Army& army, const MissionFor& mission_for) {
    WindowPlan plan;
    for (std::size_t begin = 0; begin < transcript.sentences.size();) {
        const std::size_t number = plan.missions.size() + 1;
        SentenceWindow window = window_from(transcript, begin, bytes);
        RR_TRY_ASSIGN(agents::Mission mission, mission_for(window, number));
        if (!army.holds(mission)) {
            // Bisect for the largest end the engine holds: `held` holds (or
            // is the window of one sentence, to be tried), `refused` does not.
            std::size_t held = begin + 1;
            std::size_t refused = window.end;
            while (refused - held > 1) {
                const std::size_t middle = held + ((refused - held) / 2);
                RR_TRY_ASSIGN(const agents::Mission trial,
                              mission_for(window_of(transcript, begin, middle), number));
                if (army.holds(trial)) {
                    held = middle;
                } else {
                    refused = middle;
                }
            }
            window = window_of(transcript, begin, held);
            RR_TRY_ASSIGN(mission, mission_for(window, number));
        }
        if (army.holds(mission)) {
            plan.windows.push_back(window);
            plan.missions.push_back(std::move(mission));
        } else {
            plan.too_long.push_back(window);
        }
        begin = window.end;
    }
    return plan;
}

}  // namespace rr::pipeline
