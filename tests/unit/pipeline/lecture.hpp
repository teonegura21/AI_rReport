// lecture.hpp — a synthetic fifteen-minute lecture, written for the oracle's
// rules (rr/oracle/answers.hpp): its transcript, source, outline and the
// ledger and plan the oracle makes of it. What the tests of the stages that
// read atoms run on: atomize writes the ledger, plan reads it, write reads both.
#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <rr/core/time_span.hpp>
#include <rr/json/json.hpp>
#include <rr/knowledge/atoms.hpp>
#include <rr/knowledge/outline.hpp>
#include <rr/knowledge/plan.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/oracle/oracle.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/testkit/result.hpp>

namespace rr::pipeline::testing {

// Thirty sentences, one every 30 s, each said for 25 s: fifteen minutes,
// written for the oracle's rule (rr/oracle/answers.hpp). A greeting at either
// end and a disfluency at S3; the NRC lends 3 scales at S4; hedges at S5 and
// S29, which names Pierre; one of the video's words at S6; S7 runs on into
// S8; a name, a year and words to quote at S9; a question at S10; samples
// weighed in between.
inline std::vector<std::string> lecture_texts() {
    std::vector<std::string> texts;
    texts.reserve(30);
    for (int number = 1; number <= 30; ++number) {
        texts.push_back(std::format(
            "Sample {} glowed faintly in the dark of the shed, and Ada wrote down carefully what the old "
            "scales said.",
            number));
    }
    texts[0] = "Hello and welcome back.";
    texts[1] = "I'm Ada, and today we weigh radium.";
    texts[2] = "Um, so.";
    texts[3] = "The NRC lent us 3 scales for it.";
    texts[4] = "Maybe the scales are older than the lab.";
    texts[5] = "Its half-life is long.";
    texts[6] = "Then we added a second pan, and the weight it held";
    texts[7] = "doubled.";
    texts[8] = "In 1902 Marie Curie called radium \"the most precious thing\".";
    texts[9] = "Why does it glow?";
    texts[28] = "I think the numbers mean we need better scales, Pierre.";
    texts[29] = "Thanks for watching.";
    return texts;
}

inline knowledge::Transcript transcript_of(const std::vector<std::string>& texts) {
    knowledge::Transcript transcript;
    transcript.sentences.reserve(texts.size());
    for (std::size_t index = 0; index < texts.size(); ++index) {
        const Millis start{static_cast<std::int64_t>(index) * 30'000};
        transcript.sentences.push_back(
            knowledge::Sentence{.id = knowledge::SentenceId{static_cast<std::uint32_t>(index + 1)},
                                .text = texts[index],
                                .span = testkit::unwrap(TimeSpan::make(start, start + Millis{25'000}))});
    }
    return transcript;
}

inline knowledge::Transcript lecture() {
    return transcript_of(lecture_texts());
}

inline Source source() {
    return Source{.origin = Origin::web,
                  .video = media::Video{.id = "abc123XYZ_0",
                                        .title = "Weighing radium",
                                        .description = "A lecture from the shed.",
                                        .channel = "Science Hour"},
                  .captions = SourceCaptions{.language = "en",
                                             .kind = media::CaptionKind::manual,
                                             .format = transcript::CaptionFormat::vtt,
                                             .content = "WEBVTT\n"}};
}

// The outline stage's artifact for the lecture: three sections, and its brief.
inline knowledge::Outline lecture_outline() {
    return knowledge::Outline{
        .sections = {knowledge::Section{.first = knowledge::SentenceId{1},
                                        .last = knowledge::SentenceId{10},
                                        .title = "Weighing radium"},
                     knowledge::Section{.first = knowledge::SentenceId{11},
                                        .last = knowledge::SentenceId{20},
                                        .title = "The samples"},
                     knowledge::Section{.first = knowledge::SentenceId{21},
                                        .last = knowledge::SentenceId{30},
                                        .title = "What they mean"}},
        .brief =
            knowledge::Brief{.genre = knowledge::Genre::talk,
                             .speakers = {knowledge::Speaker{.name = "Ada Lovelace", .role = "lecturer"}},
                             .lexicon = {knowledge::LexiconTerm{.term = "NRC"},
                                         knowledge::LexiconTerm{.term = "half-life",
                                                                .meaning = "how long it takes to lose half"}},
                             .gist = "Ada weighs radium, sample by sample."},
        .sections_from = knowledge::SectionsFrom::models};
}

// Renumbers `ledger`'s atoms A1, A2, … in the order they are.
inline knowledge::AtomLedger renumbered(knowledge::AtomLedger ledger) {
    for (std::size_t index = 0; index < ledger.atoms.size(); ++index) {
        ledger.atoms[index].id = knowledge::AtomId{static_cast<std::uint32_t>(index + 1)};
    }
    return ledger;
}

// What the oracle makes of `transcript`, the lecture or one like it: an atom
// a sentence, the sentence itself, but for the fillers; S8 told with S7 when
// S7 runs on.
inline knowledge::AtomLedger ledger_of(const knowledge::Transcript& transcript) {
    knowledge::AtomLedger ledger;
    const auto add = [&](const std::vector<std::uint32_t>& sentences, knowledge::AtomKind kind,
                         knowledge::Modality modality) {
        knowledge::Atom atom{
            .id = knowledge::AtomId{1}, .kind = kind, .modality = modality};  // numbered at the end
        for (const std::uint32_t number : sentences) {
            atom.sentences.emplace_back(number);
            atom.text += (atom.text.empty() ? "" : " ") + transcript.sentences[number - 1].text;
        }
        ledger.atoms.push_back(std::move(atom));
        return ledger.atoms.size() - 1;
    };
    ledger.atoms.reserve(transcript.sentences.size());
    ledger.atoms[add({2}, knowledge::AtomKind::claim, knowledge::Modality::asserted)].names = {"Ada"};
    ledger.atoms[add({4}, knowledge::AtomKind::number, knowledge::Modality::asserted)].terms = {"NRC"};
    add({5}, knowledge::AtomKind::claim, knowledge::Modality::hedged);
    add({6}, knowledge::AtomKind::claim, knowledge::Modality::asserted);
    if (transcript.sentences[6].text.ends_with('.')) {
        add({7}, knowledge::AtomKind::claim, knowledge::Modality::asserted);
        add({8}, knowledge::AtomKind::claim, knowledge::Modality::asserted);
    } else {
        add({7, 8}, knowledge::AtomKind::claim, knowledge::Modality::asserted);
    }
    knowledge::Atom& quoting =
        ledger.atoms[add({9}, knowledge::AtomKind::number, knowledge::Modality::asserted)];
    quoting.names = {"Marie Curie"};
    quoting.must_quote = true;
    quoting.quote = "the most precious thing";
    add({10}, knowledge::AtomKind::question, knowledge::Modality::question);
    for (std::uint32_t number = 11; number <= 28; ++number) {
        ledger.atoms[add({number}, knowledge::AtomKind::number, knowledge::Modality::asserted)].names = {
            "Ada"};
    }
    ledger.atoms[add({29}, knowledge::AtomKind::claim, knowledge::Modality::hedged)].names = {"Pierre"};
    ledger.fillers = {
        knowledge::Filler{.sentence = knowledge::SentenceId{1}, .label = knowledge::FillerLabel::greeting},
        knowledge::Filler{.sentence = knowledge::SentenceId{3}, .label = knowledge::FillerLabel::disfluency},
        knowledge::Filler{.sentence = knowledge::SentenceId{30}, .label = knowledge::FillerLabel::greeting}};
    return renumbered(std::move(ledger));
}

inline knowledge::AtomLedger lecture_ledger() {
    return ledger_of(lecture());
}

// `ledger` without what it made of the sentences `numbers`.
inline knowledge::AtomLedger without(knowledge::AtomLedger ledger,
                                     const std::vector<std::uint32_t>& numbers) {
    const auto among = [&](knowledge::SentenceId id) { return std::ranges::contains(numbers, id.number()); };
    std::erase_if(ledger.atoms, [&](const knowledge::Atom& atom) { return among(atom.sentences.front()); });
    std::erase_if(ledger.fillers, [&](const knowledge::Filler& filler) { return among(filler.sentence); });
    return renumbered(std::move(ledger));
}

// A fault that changes the answer to the first call of `mission` as `change`
// says, the answer read as JSON.
inline oracle::Fault reworded(std::string mission, std::function<void(json::Json&)> change, int calls = 1) {
    return oracle::edit("reworded", oracle::When{.mission = std::move(mission), .calls = calls},
                        [change = std::move(change)](const std::string& answer) {
                            json::Json read = testkit::unwrap(json::parse(answer));
                            change(read);
                            return json::dump(read);
                        });
}

// A1…An, from `first` to `last`.
inline std::vector<knowledge::AtomId> atom_ids(std::uint32_t first, std::uint32_t last) {
    std::vector<knowledge::AtomId> ids;
    for (std::uint32_t number = first; number <= last; ++number) {
        ids.emplace_back(number);
    }
    return ids;
}

// A paragraph as the oracle plans it, its atoms `first` to `last`: "Tells A1 to A3.".
inline knowledge::Paragraph oracle_paragraph(std::uint32_t first, std::uint32_t last) {
    const std::string purpose =
        first == last ? std::format("Tells A{}.", first)
                      : std::format("Tells A{} {} A{}.", first, last == first + 1 ? "and" : "to", last);
    return knowledge::Paragraph{.purpose = purpose, .atoms = atom_ids(first, last)};
}

// The plan the oracle makes of the lecture's ledger: seven atoms in "Weighing
// radium" (A1–A7), ten in "The samples" (A8–A17), nine in "What they mean"
// (A18–A26), told three a paragraph, each section headed by its title.
inline knowledge::Plan lecture_plan() {
    using knowledge::PlannedBy;
    using knowledge::SectionPlan;
    using knowledge::SentenceId;
    return knowledge::Plan{
        .sections = {SectionPlan{.first = SentenceId{1},
                                 .heading = "Weighing radium",
                                 .paragraphs = {oracle_paragraph(1, 3), oracle_paragraph(4, 6),
                                                oracle_paragraph(7, 7)},
                                 .planned_by = PlannedBy::commander},
                     SectionPlan{.first = SentenceId{11},
                                 .heading = "The samples",
                                 .paragraphs = {oracle_paragraph(8, 10), oracle_paragraph(11, 13),
                                                oracle_paragraph(14, 16), oracle_paragraph(17, 17)},
                                 .planned_by = PlannedBy::commander},
                     SectionPlan{.first = SentenceId{21},
                                 .heading = "What they mean",
                                 .paragraphs = {oracle_paragraph(18, 20), oracle_paragraph(21, 23),
                                                oracle_paragraph(24, 26)},
                                 .planned_by = PlannedBy::commander}}};
}

}  // namespace rr::pipeline::testing
