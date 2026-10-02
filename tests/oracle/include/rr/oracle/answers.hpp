// rr/oracle/answers.hpp — the right answers to the pipeline's missions, as the
// oracle computes them from a request.
//
// Why this exists
//   The oracle (rr/oracle/oracle.hpp) answers a mission kind once a stage
//   teaches it how. The answerers live here, beside it, so that every test
//   that runs stages, and the fake engine of the end-to-end tests, teach it
//   the same way (teach_stages()). Each stage adds its kind as it lands.
//
// Punctuation
//   The right punctuation of words said without any is not in the words: a
//   model guesses it, well. A synthetic video is written as a script first,
//   and its automatic captions are the script's words, lowercased and
//   stripped of punctuation. So the oracle punctuates from the script: it
//   finds the excerpt's words in it and answers with the script's text for
//   them. Without a script, it ends a sentence every twelve words: an answer
//   that keeps every word and passes the checks, so a pipeline runs end to
//   end on any captions.
//
// Annotation, outlines and descriptions
//   The eval's silver drafts (rr/eval/annotate.hpp), a video's outline and
//   its sections' descriptions (rr/pipeline/outline_stage.hpp) have no
//   single right answer either. The oracle answers each by a rule a test can
//   predict, and that keeps every check: see annotate(), outline(),
//   describe() and brief(). A synthetic video is written for the rule: its
//   talk turns at a sentence that opens with "Now," or "Next,"; a speaker
//   says "I'm Ada"; its words for things are acronyms.
//
// Thread-safety: answerers are immutable and safe to call from many threads.
#pragma once

#include <optional>
#include <string>

#include <rr/core/result.hpp>
#include <rr/llm/chat.hpp>
#include <rr/oracle/oracle.hpp>

namespace rr::oracle {

// The excerpt of a punctuate mission: what the last of its messages that
// holds one has between "<excerpt>\n" and "\n</excerpt>"
// (prompts/punctuate/task.md.inja). Errc::invalid_argument when none does.
[[nodiscard]] Result<std::string> excerpt_in(const llm::ChatRequest& request);

// Answers punctuate missions with {"text": …} (see the header comment).
// With a script, an excerpt whose words are not in it is
// Errc::invalid_argument: the oracle never guesses.
[[nodiscard]] Answerer punctuate(std::optional<std::string> script = std::nullopt);

// Answers annotate missions (prompts/annotate) the way a careful but literal
// reader would: every sentence of five words or more to annotate is a fact,
// quoted whole, core when it holds a digit. No must-quote, no term. An
// answer that passes every check, and that a test can predict.
[[nodiscard]] Answerer annotate();

// Answers annotate_more missions: every sentence of three words or more that
// the notes (<found>) do not cite is a detail fact, quoted whole.
[[nodiscard]] Answerer annotate_more();

// Answers outline missions (prompts/outline): a section starts at S1 and at
// each sentence to outline that opens with "Now," or "Next,", titled by its
// first five words, without the punctuation after the fifth.
[[nodiscard]] Answerer outline();

// Answers describe missions (prompts/describe): a speaker for each "I'm" and
// the capitalized name after it, without a role; a word for each acronym
// (two capitals or more), without a meaning, once; the section's first
// sentence as its gist; and "talk".
[[nodiscard]] Answerer describe();

// Answers brief missions (prompts/brief): "A video in 3 sections, from
// <the first's title> to <the last's>."
[[nodiscard]] Answerer brief();

// Teaches `oracle` every mission kind the pipeline's stages send, punctuating
// from `script` when there is one.
void teach_stages(Oracle& oracle, std::optional<std::string> script = std::nullopt);

}  // namespace rr::oracle
