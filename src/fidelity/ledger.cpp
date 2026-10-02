// Ledger A: the atoms against the transcript (rr/fidelity/checks.hpp).
#include <format>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <rr/fidelity/checks.hpp>
#include <rr/text/align.hpp>
#include <rr/text/entities.hpp>

#include "held.hpp"

namespace rr::fidelity {

namespace {

using knowledge::Atom;
using knowledge::AtomId;
using knowledge::AtomLedger;
using knowledge::FillerLabel;
using knowledge::SentenceId;
using knowledge::Transcript;

Finding finding(Check check, Severity severity, std::string message, std::vector<SentenceId> sentences,
                std::vector<AtomId> atoms) {
    return Finding{.check = check,
                   .severity = severity,
                   .message = std::move(message),
                   .sentences = std::move(sentences),
                   .atoms = std::move(atoms)};
}

// "disfluency, greeting, …": the closed list of ADR-0005, for messages.
std::string filler_labels() {
    std::string labels;
    for (int label = 0; label <= static_cast<int>(FillerLabel::decorative); ++label) {
        labels +=
            std::string{labels.empty() ? "" : ", "} + std::string{to_string(static_cast<FillerLabel>(label))};
    }
    return labels;
}

// Every sentence of the transcript became atoms or a filler label, not both;
// every sentence named is the transcript's.
void account_for_sentences(const Transcript& transcript, const AtomLedger& ledger, Report& report) {
    std::map<SentenceId, std::vector<AtomId>> told;
    for (const Atom& atom : ledger.atoms) {
        for (const SentenceId& sentence : atom.sentences) {
            told[sentence].push_back(atom.id);
        }
    }
    std::map<SentenceId, FillerLabel> fillers;
    for (const knowledge::Filler& filler : ledger.fillers) {
        fillers.emplace(filler.sentence, filler.label);
    }
    std::set<SentenceId> known;
    report.coverage.sentences += transcript.sentences.size();
    for (const knowledge::Sentence& sentence : transcript.sentences) {
        known.insert(sentence.id);
        const auto atoms = told.find(sentence.id);
        const auto filler = fillers.find(sentence.id);
        if (atoms != told.end() && filler != fillers.end()) {
            report.findings.push_back(
                finding(Check::sentence_once, Severity::error,
                        std::format("{} is labelled {} and told by {}: it is one or the other", sentence.id,
                                    to_string(filler->second), held::listed(atoms->second)),
                        {sentence.id}, atoms->second));
        } else if (atoms == told.end() && filler == fillers.end()) {
            report.findings.push_back(finding(
                Check::sentence_accounted, Severity::error,
                std::format("{} is not accounted for: give it one or more atoms, or a filler label ({})",
                            sentence.id, filler_labels()),
                {sentence.id}, {}));
        } else {
            ++report.coverage.sentences_accounted;
        }
    }
    for (const Atom& atom : ledger.atoms) {
        std::vector<SentenceId> unknown;
        std::ranges::copy_if(atom.sentences, std::back_inserter(unknown),
                             [&](const SentenceId& sentence) { return !known.contains(sentence); });
        if (!unknown.empty()) {
            report.findings.push_back(
                finding(Check::sentence_known, Severity::error,
                        std::format("{} cites {}, which the transcript given has not: cite "
                                    "only the sentences you were given",
                                    atom.id, held::listed(unknown)),
                        unknown, {atom.id}));
        }
    }
    for (const knowledge::Filler& filler : ledger.fillers) {
        if (!known.contains(filler.sentence)) {
            report.findings.push_back(finding(
                Check::sentence_known, Severity::error,
                std::format("a filler labels {}, which the transcript given has not", filler.sentence),
                {filler.sentence}, {}));
        }
    }
}

// "\"37%\", \"2023\"": what a text says instead, for a message.
std::string stated(std::string_view text, const std::vector<text::NumberMention>& numbers) {
    constexpr std::size_t kShown = 5;
    if (numbers.empty()) {
        return "no number";
    }
    std::string list;
    for (std::size_t index = 0; index < numbers.size() && index < kShown; ++index) {
        list += std::format("{}\"{}\"", list.empty() ? "" : ", ", text::text_of(text, numbers[index].span));
    }
    return numbers.size() > kShown ? list + ", \u2026" : list;
}

// An atom's numbers and must-quote were said in its sentences; its names
// somewhere in the transcript given.
void ground_atom(const Transcript& transcript, std::string_view whole, const Atom& atom, Report& report) {
    const std::string sources = held::said(transcript, atom.sentences);
    const std::vector<text::NumberMention> said = held::numbers(sources);
    for (const text::NumberMention& number : held::numbers(atom.text)) {
        if (!held::states(said, number)) {
            report.findings.push_back(
                finding(Check::number_said, Severity::error,
                        std::format("{} says \"{}\", which its sentences ({}) do not: they say {}", atom.id,
                                    text::text_of(atom.text, number.span), held::listed(atom.sentences),
                                    stated(sources, said)),
                        atom.sentences, {atom.id}));
        }
    }
    if (atom.quote.has_value()) {
        const text::Alignment found = text::align(*atom.quote, sources);
        if (!found.verbatim()) {
            const std::optional<std::string> said_instead = held::closest(found, sources);
            const std::string message =
                said_instead.has_value()
                    ? std::format(R"({}'s quote "{}" is not what was said: its sentences say "{}")", atom.id,
                                  held::excerpt(*atom.quote), *said_instead)
                    : std::format("{}'s quote \"{}\" is nowhere in its sentences ({})", atom.id,
                                  held::excerpt(*atom.quote), held::listed(atom.sentences));
            report.findings.push_back(
                finding(Check::quote_said, Severity::error, message, atom.sentences, {atom.id}));
        }
    }
    for (const std::string& name : atom.names) {
        if (!held::names(whole, name)) {
            report.findings.push_back(finding(
                Check::name_said, Severity::warning,
                std::format("{} names \"{}\", whom the transcript given does not name: check it was said",
                            atom.id, name),
                atom.sentences, {atom.id}));
        }
    }
}

// What a filler sentence holds that a filler must not: a number, a name, a
// lexicon term (ADR-0005's hard rule). Empty when it holds none.
std::string signal_in(std::string_view sentence, const std::set<std::string>& lexicon) {
    if (const std::vector<text::NumberMention> numbers = held::numbers(sentence); !numbers.empty()) {
        return std::format("the number \"{}\"", text::text_of(sentence, numbers.front().span));
    }
    if (const std::vector<text::Span> names = text::names(sentence); !names.empty()) {
        return std::format("the name \"{}\"", text::text_of(sentence, names.front()));
    }
    for (const std::string& term : lexicon) {
        if (held::uses(sentence, term)) {
            return std::format("the term \"{}\"", term);
        }
    }
    return {};
}

void check_fillers(const Transcript& transcript, const AtomLedger& ledger, std::span<const std::string> video,
                   Report& report) {
    std::set<std::string> lexicon(video.begin(), video.end());
    std::set<SentenceId> told;
    for (const Atom& atom : ledger.atoms) {
        lexicon.insert(atom.terms.begin(), atom.terms.end());
        told.insert(atom.sentences.begin(), atom.sentences.end());
    }
    for (const knowledge::Filler& filler : ledger.fillers) {
        const knowledge::Sentence* sentence = transcript.find(filler.sentence);
        if (sentence == nullptr || told.contains(filler.sentence)) {
            continue;  // sentence-known and sentence-once say so
        }
        if (const std::string signal = signal_in(sentence->text, lexicon); !signal.empty()) {
            report.findings.push_back(finding(
                Check::filler_empty, Severity::error,
                std::format("{} is labelled {}, yet holds {}: re-examine it, and give it atoms unless it "
                            "carries nothing",
                            filler.sentence, to_string(filler.label), signal),
                {filler.sentence}, {}));
        }
    }
}

}  // namespace

Report check_ledger(const Transcript& transcript, const AtomLedger& ledger,
                    std::span<const std::string> lexicon) {
    Report report;
    account_for_sentences(transcript, ledger, report);
    std::vector<SentenceId> all;
    all.reserve(transcript.sentences.size());
    for (const knowledge::Sentence& sentence : transcript.sentences) {
        all.push_back(sentence.id);
    }
    const std::string whole = held::said(transcript, all);
    for (const Atom& atom : ledger.atoms) {
        ground_atom(transcript, whole, atom, report);
    }
    check_fillers(transcript, ledger, lexicon, report);
    return report;
}

}  // namespace rr::fidelity
