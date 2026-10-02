// rr/pipeline/windows.hpp — a transcript cut into windows of whole sentences,
// each as large as the engine that reads it holds.
//
// Why this exists
//   A stage that asks models about a transcript asks a window at a time: a
//   model reads a few pages better than a book, and an engine holds only so
//   many tokens (rr/agents/sizing.hpp). Every such stage cuts the same way:
//     * a window is whole sentences [begin, end), shown a line each as
//       "S12 [3:04] What was said." (sentence_lines());
//     * it is sent with a little context on either side, up to three
//       sentences, as many of them as fit 1,000 bytes of lines, so that its
//       first and last sentences are read as they were said;
//     * it is filled with sentences up to a size the stage chooses, then,
//       while the engine would not hold its mission (agents::Army::holds(),
//       the very test the squad applies), it gives up sentences from its end:
//       the largest window the engine holds, found by bisection. So no
//       mission is ever refused for its size, whatever the prompt around the
//       sentences, and a sentence too long for its engine even alone is set
//       aside, for the stage to report.
//   Holding is monotone in a window's end: giving up a sentence takes its
//   line out of the prompt and puts at most one line of context in, and the
//   answer a window needs shrinks with it. That is what makes bisection find
//   the same window that giving up one sentence at a time would.
//
// Thread-safety: pure functions; plan_windows() calls `make` and the army's
//   holds() from the calling thread.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <rr/agents/army.hpp>
#include <rr/agents/mission.hpp>
#include <rr/core/result.hpp>
#include <rr/knowledge/transcript.hpp>

namespace rr::pipeline {

// A window of the transcript: its own sentences [begin, end), and with its
// context [context_begin, context_end), as indices of transcript.sentences.
struct SentenceWindow {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t context_begin = 0;
    std::size_t context_end = 0;

    friend bool operator==(const SentenceWindow&, const SentenceWindow&) = default;
};

// A window's context, on either side: up to three sentences, as many of them
// as fit 1,000 bytes of lines.
inline constexpr std::size_t kContextSentences = 3;
inline constexpr std::size_t kContextBytes = 1'000;

// Sentences [begin, end) as the prompts show them, a line each:
// "S12 [3:04] What was said." Indices past the transcript's end are left out.
[[nodiscard]] std::string sentence_lines(const knowledge::Transcript& transcript, std::size_t begin,
                                         std::size_t end);

// "S12–S48", or "S12" alone: the sentences [begin, end), which are one at
// least, named.
[[nodiscard]] std::string sentence_range(std::size_t begin, std::size_t end);

// How a finding about the sentences [begin, end) of `transcript` opens:
// "the sentences S3–S6 (0:06 to 0:40) have", or "the sentence S3 (0:06 to
// 0:40) has" alone.
[[nodiscard]] std::string sentences_have(const knowledge::Transcript& transcript, std::size_t begin,
                                         std::size_t end);

// The bytes the sentences of `window` take in a prompt: their lines, each
// with its newline.
[[nodiscard]] std::size_t lines_bytes(const knowledge::Transcript& transcript, const SentenceWindow& window);

// The window of the sentences [begin, end), which must be some of
// `transcript`'s, with its context: on either side, the nearest sentences,
// up to `context` of them, as many as fit kContextBytes of lines.
[[nodiscard]] SentenceWindow window_of(const knowledge::Transcript& transcript, std::size_t begin,
                                       std::size_t end, std::size_t context = kContextSentences);

// The window from sentence `begin`, which must be one of `transcript`'s: as
// many whole sentences as fit `bytes` of their lines, one at least, with its
// context (window_of()).
[[nodiscard]] SentenceWindow window_from(const knowledge::Transcript& transcript, std::size_t begin,
                                         std::size_t bytes, std::size_t context = kContextSentences);

// Builds the mission that reads `window`, the `number`th (from 1) of its
// reading. Errors: the mission's own, such as its prompt's.
using MissionFor = std::function<Result<agents::Mission>(const SentenceWindow& window, std::size_t number)>;

// One reading of the transcript, window by window.
struct WindowPlan {
    std::vector<SentenceWindow> windows{};    // in order
    std::vector<agents::Mission> missions{};  // the windows' own, in the same order
    std::vector<SentenceWindow> too_long{};   // a sentence its engine cannot hold even alone
};

// Cuts `transcript` into windows (see the header comment): each filled to
// `bytes` of lines, then the largest that `army` holds the mission
// `mission_for` builds for, with up to `context` sentences of context on
// either side. Every sentence is in exactly one window, of `windows` or of
// `too_long`. Errors: mission_for()'s.
[[nodiscard]] Result<WindowPlan> plan_windows(const knowledge::Transcript& transcript, std::size_t bytes,
                                              const agents::Army& army, const MissionFor& mission_for,
                                              std::size_t context = kContextSentences);

}  // namespace rr::pipeline
