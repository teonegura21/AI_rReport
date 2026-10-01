// rr/pipeline/transcript_stage.hpp — the second stage: the source's captions,
// as the numbered sentences everything after answers to.
//
// Why this exists
//   The transcript is the closed world of the job (ADR-0005): sentences
//   S1…Sn, each with when it was said. This stage reads the source's
//   captions (rr/transcript/captions.hpp) and cuts them into sentences
//   (rr/transcript/sentences.hpp). Captions that are not punctuated, as
//   YouTube's automatic ones often are not, are punctuated first by the
//   infantry, a window at a time, each answer laid onto the captions' words
//   without changing one (rr/pipeline/punctuation.hpp).
//
// What it does, by the captions
//   * Punctuated captions are cut into sentences as they are. No model is
//     asked, and the pod is never started for them.
//   * Unpunctuated captions, with no pod configured: cut where the speaker
//     paused, at most 60 words a sentence, with a finding that says so.
//   * Unpunctuated captions, with a pod: each window is a "punctuate"
//     mission (prompts/punctuate), sent all at once to the army, which runs
//     as many side by side as the infantry holds. Its answer must keep the
//     words and mark sentence ends, or it is sent back, then escalated
//     (rr/agents/ladder.hpp). A window no model punctuated keeps the
//     captions' form, and a finding names the minutes it covers. A window a
//     job ceiling stopped makes the result this job's alone (StageOutput).
//
// Its key holds how the captions were punctuated: by themselves, by no one,
// or by which prompt and models (army_fingerprint()). Punctuation does not
// sample (temperature 0), so the job's salt is not in it: every job, and
// every replicate of the eval, reads one transcript of a video.
//
// Thread-safety: immutable; run() may be called from any thread.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <rr/pipeline/punctuation.hpp>
#include <rr/pipeline/stage.hpp>

namespace rr::pipeline {

class TranscriptStage final : public Stage {
public:
    static constexpr std::string_view kName = "transcript";
    static constexpr std::string_view kPrompt = "punctuate";

    explicit TranscriptStage(WindowSize window = {}) : window_(window) {}

    [[nodiscard]] std::string_view name() const override { return kName; }
    [[nodiscard]] int version() const override { return 1; }
    [[nodiscard]] std::vector<std::string> inputs() const override { return {"source"}; }
    [[nodiscard]] std::string_view schema() const override { return "artifacts/transcript"; }

    // {"punctuation": "captions"}, {"punctuation": "none"}, or
    // {"punctuation": "models", "prompt", "prompt_hash", "window", "army"},
    // for which it raises the army. Errors: captions that cannot be read
    // (Errc::invalid_argument), the army's (RaiseArmy), a prompt library
    // without the punctuate prompt (Errc::not_found).
    [[nodiscard]] Result<json::Json> fingerprint(const StageContext& context) const override;

    // Errors: as fingerprint(), and the army's (Errc::cancelled on a stop, a
    // pod that stays unreachable).
    [[nodiscard]] Result<StageOutput> run(const StageContext& context) const override;

private:
    WindowSize window_;
};

}  // namespace rr::pipeline
