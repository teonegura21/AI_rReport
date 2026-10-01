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

// Teaches `oracle` every mission kind the pipeline's stages send, punctuating
// from `script` when there is one.
void teach_stages(Oracle& oracle, std::optional<std::string> script = std::nullopt);

}  // namespace rr::oracle
