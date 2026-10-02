// What the stages that write prose share (rr/pipeline/drafting.hpp).
#include <rr/pipeline/drafting.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include <rr/core/assert.hpp>
#include <rr/core/time_span.hpp>
#include <rr/fidelity/checks.hpp>
#include <rr/knowledge/article.hpp>
#include <rr/pipeline/asking.hpp>
#include <rr/text/span.hpp>
#include <rr/text/unicode.hpp>

namespace rr::pipeline {

namespace {

using knowledge::Atom;
using knowledge::AtomId;
using knowledge::Transcript;

constexpr int kTokensPerParagraph = 64;   // a paragraph's markers and breaks
constexpr std::size_t kShownBytes = 120;  // of a sentence a message quotes

// The draft's blocks, read: its paragraphs, and whether it wrote anything else.
struct Read {
    knowledge::ParsedArticle parsed{};
    std::vector<const knowledge::Block*> paragraphs{};
    bool headed = false;
    bool other = false;  // a list, a quote set apart, code
};

Read read_draft(std::string_view draft) {
    Read read{.parsed = knowledge::parse_article(draft)};
    for (const knowledge::Block& block : read.parsed.blocks) {
        switch (block.kind) {
            case knowledge::BlockKind::paragraph:
                read.paragraphs.push_back(&block);
                break;
            case knowledge::BlockKind::heading:
                read.headed = true;
                break;
            case knowledge::BlockKind::list_item:
            case knowledge::BlockKind::quote:
            case knowledge::BlockKind::code:
                read.other = true;
                break;
        }
    }
    return read;
}

// The paragraphs, of `read`'s, that cite `atom`, in order.
std::vector<std::size_t> paragraphs_citing(const Read& read, AtomId atom) {
    std::vector<std::size_t> citing;
    for (std::size_t index = 0; index < read.paragraphs.size(); ++index) {
        const auto cites = [&](const knowledge::CitingSentence& sentence) {
            return std::ranges::any_of(sentence.markers,
                                       [&](const knowledge::Marker& marker) { return marker.atom == atom; });
        };
        if (std::ranges::any_of(read.paragraphs[index]->sentences, cites)) {
            citing.push_back(index);
        }
    }
    return citing;
}

void check_shape(const Read& read, const Asked& asked, Problems& problems) {
    if (read.headed) {
        problems.note("write no heading: the article has its own");
    }
    if (read.other) {
        problems.note("write paragraphs of prose only: no lists, no quotes set apart, no code");
    }
    if (read.paragraphs.size() != asked.paragraphs.size()) {
        problems.note(std::format("write {} paragraphs, one for each given, not {}", asked.paragraphs.size(),
                                  read.paragraphs.size()));
        return;  // which paragraph is which cannot be told
    }
    // Each atom is told in its own paragraph, and in no other: a paragraph
    // that tells another's atom again tells it twice, and across a seam
    // under another section's heading.
    for (std::size_t index = 0; index < asked.paragraphs.size(); ++index) {
        for (const AtomId atom : asked.paragraphs[index]) {
            const std::vector<std::size_t> citing = paragraphs_citing(read, atom);
            const auto other =
                std::ranges::find_if(citing, [&](std::size_t found) { return found != index; });
            if (other == citing.end()) {
                continue;  // told in its own paragraph only, or not at all, which Ledger B says
            }
            if (std::ranges::find(citing, index) == citing.end()) {
                problems.note(std::format("{} is told in paragraph {}: tell it in paragraph {}, as given",
                                          atom, asked.first + *other, asked.first + index));
            } else {
                problems.note(std::format("{} is told again in paragraph {}: tell it in paragraph {} only",
                                          atom, asked.first + *other, asked.first + index));
            }
        }
    }
}

}  // namespace

std::int64_t atom_bytes(const Atom& atom) {
    return static_cast<std::int64_t>(atom.text.size() + atom.quote.value_or("").size());
}

int prose_tokens(std::int64_t atom_bytes, std::size_t paragraphs) {
    const std::int64_t tokens =
        ((atom_bytes * 2) / 3) + (static_cast<std::int64_t>(paragraphs) * kTokensPerParagraph) + kSpareTokens;
    return static_cast<int>(std::clamp<std::int64_t>(tokens, kLeastAnswer, kMostAnswer));
}

std::string shown(std::string_view text) {
    if (text.size() <= kShownBytes) {
        return std::string{text};
    }
    std::size_t end = 0;
    for (const text::CodePoint& point : text::code_points(text)) {
        if (point.end > kShownBytes) {
            break;
        }
        end = point.end;
    }
    return std::string{text.substr(0, end)} + "\u2026";
}

std::string atom_entry(const Atom& atom, const Transcript& transcript) {
    const knowledge::Sentence* said = transcript.find(atom.sentences.front());
    RR_ASSERT(said != nullptr, "an atom is said in its transcript (read_input())");
    const std::string_view kind = knowledge::to_string(atom.kind);
    std::string labels{kind};
    const auto label = [&](std::string_view more) {
        if (more != kind) {
            labels += std::format(", {}", more);
        }
    };
    if (atom.modality != knowledge::Modality::asserted) {
        label(knowledge::to_string(atom.modality));
    }
    if (atom.register_kind != knowledge::Register::main) {
        label(knowledge::to_string(atom.register_kind));
    }
    if (atom.intensity != knowledge::Intensity::medium) {
        label(knowledge::to_string(atom.intensity));
    }
    if (atom.speaker.has_value()) {
        labels += ", said by " + *atom.speaker;
    }
    if (atom.attributed_to.has_value()) {
        labels += ", reporting " + *atom.attributed_to;
    }
    std::string entry =
        std::format("{} [{}] {}: {}", atom.id, format_timestamp(said->span.start()), labels, atom.text);
    if (atom.quote.has_value()) {
        entry += std::format(" To quote: \"{}\".", *atom.quote);
    }
    const auto listed = [&](std::string_view what, const std::vector<std::string>& words) {
        if (!words.empty()) {
            std::string list;
            for (const std::string& word : words) {
                list += (list.empty() ? "" : ", ") + word;
            }
            entry += std::format(" {}: {}.", what, list);
        }
    };
    listed("Names", atom.names);
    listed("Terms", atom.terms);
    return entry;
}

std::vector<std::string> draft_problems(std::string_view draft, const Transcript& transcript,
                                        const knowledge::AtomLedger& ledger, const Asked& asked) {
    Problems problems;
    const Read read = read_draft(draft);
    check_shape(read, asked, problems);
    for (const knowledge::Block* const paragraph : read.paragraphs) {
        for (const knowledge::CitingSentence& sentence : paragraph->sentences) {
            if (sentence.markers.empty()) {
                problems.note(
                    std::format("the sentence \"{}\" tells no atom: cite the atoms it tells, or leave it out",
                                shown(knowledge::without_markers(draft, read.parsed, sentence.span))));
            }
        }
    }
    for (const fidelity::Finding& finding :
         fidelity::check_article(transcript, ledger, draft, {.told = asked.atoms}).findings) {
        if (finding.severity == fidelity::Severity::error) {
            problems.note(finding.message);
        }
    }
    return std::move(problems).list();
}

std::size_t warnings_of(std::string_view draft, const Transcript& transcript,
                        const knowledge::AtomLedger& ledger, const Asked& asked) {
    return fidelity::check_article(transcript, ledger, draft, {.told = asked.atoms}).warnings();
}

std::vector<std::string> paragraphs_of(std::string_view draft) {
    const Read read = read_draft(draft);
    std::vector<std::string> paragraphs;
    paragraphs.reserve(read.paragraphs.size());
    for (const knowledge::Block* const paragraph : read.paragraphs) {
        paragraphs.emplace_back(text::text_of(draft, paragraph->span));
    }
    return paragraphs;
}

std::string plain(std::string_view draft) {
    const knowledge::ParsedArticle parsed = knowledge::parse_article(draft);
    std::string text;
    for (const knowledge::Block& block : parsed.blocks) {
        text += (text.empty() ? "" : "\n\n") + knowledge::without_markers(draft, parsed, block.span);
    }
    return text;
}

}  // namespace rr::pipeline
