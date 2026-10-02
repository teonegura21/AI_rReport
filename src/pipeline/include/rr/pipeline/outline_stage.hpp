// rr/pipeline/outline_stage.hpp — the stage that cuts a video into sections
// and writes the brief every later mission is given.
//
// Why this exists
//   An article is written a section at a time, and every mission after this
//   stage sees its own stretch of the video and the video's brief, no more
//   (plan §3.7; rr/knowledge/outline.hpp). The stage decides two things:
//
//   Where the sections are, best first:
//     1. The creator's chapters, when the tier says to use them and the
//        video has two or more: the creator's own cut, mapped onto the
//        sentences (rr/pipeline/outline.hpp).
//     2. The commander's outlines (prompts/outline), as many as the tier's
//        samples, thinking within the tier's budget, or within half of what
//        the commander holds if that is less: a budget that left no room for
//        the transcript would leave nothing to think about. A section starts
//        where enough of them start one within a sentence or two of each
//        other (reason::boundary_consensus()): one sample's whim is not a
//        section, and the place most of them agree on is.
//     3. Without a pod: cut where the speaker paused longest.
//   Whatever made them, no section runs longer than eight minutes: a longer
//   one is cut again where the speaker paused longest.
//
//   What they say: with a pod, the commander describes each section on its
//   own (prompts/describe): its gist, who speaks in it, the words it uses for
//   things, and what the video is. The section descriptions join into the
//   brief (outline.hpp: join_briefs()), and from their gists, in order, the
//   commander writes the video's (prompts/brief). Describing is reading,
//   not judging, so it is asked without thinking: reasoning models invent
//   more when they summarize (docs/design/pipeline.md). Only the outline,
//   a judgment over the whole, thinks.
//
// Checks, and what is sent back
//   An outline that starts a section outside its own sentences, lists them
//   out of order, does not start the video at S1, titles a section in more
//   than twelve words, or cuts more than a section a minute (a section per
//   pause is no outline); a description that names a speaker neither the
//   section nor the video's description names, lists a word the section
//   does not say or more than fifteen, or gives a gist longer than two
//   sentences; a video gist longer than a paragraph. Each is sent back
//   with what to do instead (rr/agents/army.hpp: repairs and escalation).
//   What still has no answer is a finding, and the stage goes on without
//   it: a stretch without an outline is cut where the speaker paused, a
//   section without a description has no gist, a brief without a gist is
//   still a brief.
//
// Windows
//   The commander outlines the whole transcript at once when it holds it
//   (agents::Army::holds()): sections are a judgment about the whole. A
//   transcript too long for it, a 90-minute podcast with a 16K thinking
//   budget on a 48 GB pod, is read in windows as large as it holds
//   (rr/pipeline/windows.hpp), each with the sentences just before it as
//   context, each listing only the sections that start among its own.
//   Sentences too long for it even alone are cut where the speaker paused.
//
// Its key holds the tier's outline settings and the longest a section may
// run; with a pod, the prompts, the army and what the commander holds. It
// samples: the job's salt is in its key.
//
// Thread-safety: immutable.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <rr/pipeline/stage.hpp>

namespace rr::pipeline {

class OutlineStage final : public Stage {
public:
    static constexpr std::string_view kName = "outline";

    [[nodiscard]] std::string_view name() const override { return kName; }
    [[nodiscard]] int version() const override { return 1; }
    [[nodiscard]] std::vector<std::string> inputs() const override { return {"source", "transcript"}; }
    [[nodiscard]] std::string_view schema() const override { return "artifacts/outline"; }
    [[nodiscard]] bool samples() const override { return true; }

    // Errc::not_found for a job whose tier rReport does not ship; the army's
    // and the prompts' errors.
    [[nodiscard]] Result<json::Json> fingerprint(const StageContext& context) const override;

    // Errc::invalid_argument for a transcript without sentences: there is
    // nothing to outline. Errors: the army's. What no model could do is a
    // finding (see the header comment).
    [[nodiscard]] Result<StageOutput> run(const StageContext& context) const override;
};

}  // namespace rr::pipeline
