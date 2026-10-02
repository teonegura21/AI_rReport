// rr/pipeline/asking.hpp — what every stage that asks models does the same
// way: find its prompts and fingerprint them, find the engine serving a
// role, show the video in a prompt's shared part, send an answer's problems
// back a few at a time, and say why a mission ended without an answer.
//
// Why this exists
//   Stages differ in what they ask, not in how they introduce the video or
//   explain a gap. Said once, here:
//     * A prompt shows the video's title, its channel and the start of its
//       description: enough for the names and terms it spells, without the
//       links and sponsors that follow them. The start is cut after a line
//       or, failing that, after a word, never inside a character, and "…"
//       says where.
//     * An answer's problems are sent back with it a few at a time, eight at
//       most and then how many more: enough to fix, few enough to read.
//     * A mission that ended without an answer becomes a finding that says
//       why, in the reader's terms: the models declined; the job reached a
//       ceiling first; or no answer passed the stage's checks, and which.
//
// Thread-safety: pure functions.
#pragma once

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rr/agents/army.hpp>
#include <rr/core/result.hpp>
#include <rr/json/json.hpp>
#include <rr/json/schema.hpp>
#include <rr/media/video.hpp>
#include <rr/prompt/library.hpp>

namespace rr::pipeline {

// A prompt a stage asks with, and its answer's schema.
struct StagePrompt {
    const prompt::PromptInfo* info = nullptr;
    std::shared_ptr<const json::Schema> schema{};
};

// The prompt `name` of `prompts`. Errc::not_found when there is none; the
// library's errors for its schema.
[[nodiscard]] Result<StagePrompt> stage_prompt(const prompt::Library& prompts, std::string_view name);

// What a stage's results owe to the prompts `names`: each one's label
// ("outline@1") and content hash, by name. Errors: stage_prompt()'s.
[[nodiscard]] Result<json::Json> prompts_fingerprint(const prompt::Library& prompts,
                                                     std::initializer_list<std::string_view> names);

// The engine of `army` that serves `role`, or nullptr.
[[nodiscard]] const agents::Engine* engine_serving(const agents::Army& army, std::string_view role);

// The most problems sent back with an answer at once.
inline constexpr std::size_t kMostProblems = 8;

// What is wrong with an answer, each said as what to do instead (see the
// header comment).
class Problems {
public:
    void note(std::string problem);
    [[nodiscard]] bool empty() const noexcept { return problems_.empty(); }

    // The first kMostProblems noted, then "and 3 more like them".
    [[nodiscard]] std::vector<std::string> list() &&;

private:
    std::vector<std::string> problems_;
    std::size_t more_ = 0;
};

// The most of a video's description a prompt shows.
inline constexpr std::size_t kDescriptionBytes = 1'500;

// The first kDescriptionBytes of `description`, cut as the header comment
// says; `description` itself when it is no longer.
[[nodiscard]] std::string description_excerpt(std::string_view description);

// What a prompt's system part shows of `video`: "title", "channel" and
// "description" (its excerpt). The same for every mission of a stage.
[[nodiscard]] json::Json video_shared(const media::Video& video);

// Why `outcome`, which is not accepted, has no answer, for a finding:
// "the models declined to {task}", "the job reached its {kind} ceiling
// first", or "no answer passed its checks ({problems})".
[[nodiscard]] std::string why_unanswered(const agents::Outcome& outcome, const agents::Army& army,
                                         std::string_view task);

}  // namespace rr::pipeline
