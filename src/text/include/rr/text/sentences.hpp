// rr/text/sentences.hpp — a text, cut into sentences.
//
// Why this exists
//   rReport accounts for a video sentence by sentence. Every sentence of the
//   transcript is numbered (S1, S2, ...), and the article must account for
//   each (ADR-0005). Captions arrive as cues that cut sentences anywhere, so
//   the transcript is cut again here, once it is punctuated (rr_transcript
//   re-punctuates automatic captions first).
//
// The rules
//   A sentence ends after a terminator and the closing quotes and brackets
//   after it, when a space or the end of the text follows. Terminators are
//   "." "!" "?" "…" and their kin in other scripts ("‽", "؟", "।"). The
//   ideographic ones ("。", "！", "？") need no space after them.
//
//   Not every terminator ends a sentence:
//     * after "!" or "?", a lowercase word continues it: `"Why?" she asked.`;
//     * an ellipsis ends one only before a capital: "and then... we left";
//     * a period does not end one when
//         - it ends an abbreviation that never ends a sentence: "Dr. Smith",
//           "e.g. this", "Roe v. Wade";
//         - it ends one that often does ("etc.", "Inc.", "U.S."), or an
//           initial (a single capital), and the next word is no function word
//           ("the", "it", "then"; lexicon.hpp): "U.S. Army", "John F.
//           Kennedy", "J. K. Rowling". "Apple Inc. It grew" and "plan B.
//           Then" end. A wrong merge costs less than a name split in two;
//         - it ends a numbering abbreviation before a number: "No. 5", "p. 12";
//         - a lowercase word follows: "a 2 lb. bag".
//   A blank line, a paragraph break, always ends a sentence.
//
//   Nothing is lost: every character that is not a space is in exactly one
//   sentence. A fragment without a word or a number ("...", "♪") joins the
//   sentence before it, or the one after it at the start of the text.
//   Sentences are trimmed of spaces.
//
// Thread-safety: pure functions.
#pragma once

#include <string_view>
#include <vector>

#include <rr/text/span.hpp>

namespace rr::text {

// The sentences of `text`, in order (see the header comment).
[[nodiscard]] std::vector<Span> sentences(std::string_view text);

}  // namespace rr::text
