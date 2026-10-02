// The stage that writes Ledger A (rr/pipeline/atomize_stage.hpp).
#include <rr/pipeline/atomize_stage.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/agents/mission.hpp>
#include <rr/core/assert.hpp>
#include <rr/fidelity/checks.hpp>
#include <rr/knowledge/atoms.hpp>
#include <rr/knowledge/outline.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/llm/memo.hpp>
#include <rr/pipeline/asking.hpp>
#include <rr/pipeline/outline_stage.hpp>
#include <rr/pipeline/readings.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/pipeline/transcript_stage.hpp>
#include <rr/pipeline/windows.hpp>
#include <rr/reason/tier.hpp>
#include <rr/reason/vote.hpp>
#include <rr/text/align.hpp>
#include <rr/text/unicode.hpp>

namespace rr::pipeline {

namespace {

using knowledge::Atom;
using knowledge::SentenceId;
using knowledge::Transcript;

constexpr std::string_view kPrompt = "atomize";
constexpr std::size_t kWindowBytes = 2'500;  // a window's lines, filled to; then given up until held
constexpr std::size_t kContext = 2;          // sentences of context on either side (plan: ±2)
constexpr double kLeastCoverage = 0.5;       // below it, the recall pass asks a sentence again
constexpr double kRepeated = 0.25;           // a recalled atom this close to one held repeats it

// An answer's size. Its JSON takes about 300 bytes a sentence besides the
// words it restates: the row, and an atom or two with their eleven fields
// (the prompt's own example takes 1,180 bytes for five short sentences).
// Counted at the estimator's three bytes a token, which errs high, and with
// room to spare: an answer cut short is asked again, whole.
constexpr std::size_t kRowBytes = 320;
constexpr std::size_t kBytesPerToken = 3;
constexpr std::size_t kSpareTokens = 128;
constexpr std::size_t kLeastAnswer = 512;
constexpr std::size_t kMostAnswer = 8'192;

int answer_tokens(std::size_t sentences, std::size_t lines_bytes) {
    const std::size_t tokens = (((sentences * kRowBytes) + lines_bytes) / kBytesPerToken) + kSpareTokens;
    return static_cast<int>(std::clamp(tokens, kLeastAnswer, kMostAnswer));
}

std::string folded(std::string_view text) {
    return text::fold(text).value_or(std::string{text});
}

// Whether `text` says `words`, word for word (case, accents and punctuation aside).
bool says(std::string_view text, std::string_view words) {
    return text::align(words, text).verbatim();
}

// The schemas allow S1, S2, … only.
SentenceId sentence_of(const json::Json& written) {
    const std::optional<SentenceId> id = SentenceId::parse(written.get<std::string>());
    RR_ASSERT(id.has_value(), "the answer's schema allows S1, S2, \u2026 only");
    return *id;
}

// Where the sentence `id` is in the transcript's sentences: S1 is the first.
std::size_t index_of(SentenceId id) {
    return id.number() - std::size_t{1};
}

// The sentences of `transcript` at `indices`, in order, as a transcript of
// their own: what an answer about them is checked against.
Transcript part_of(const Transcript& transcript, std::span<const std::size_t> indices) {
    Transcript part;
    part.sentences.reserve(indices.size());
    for (const std::size_t index : indices) {
        part.sentences.push_back(transcript.sentences[index]);
    }
    return part;
}

// The indices [begin, end).
std::vector<std::size_t> indices_of(std::size_t begin, std::size_t end) {
    std::vector<std::size_t> indices(end - begin);
    std::ranges::iota(indices, begin);
    return indices;
}

std::vector<SentenceId> ids_of(const Transcript& part) {
    std::vector<SentenceId> ids;
    ids.reserve(part.sentences.size());
    for (const knowledge::Sentence& sentence : part.sentences) {
        ids.push_back(sentence.id);
    }
    return ids;
}

// "S3–S9", or "S3, S7, S9" when they do not follow one another.
std::string named(std::span<const SentenceId> ids) {
    const bool following = std::ranges::adjacent_find(ids, [](SentenceId lhs, SentenceId rhs) {
                               return rhs.number() != lhs.number() + 1;
                           }) == ids.end();
    if (following && !ids.empty()) {
        return sentence_range(index_of(ids.front()), index_of(ids.back()) + 1);
    }
    std::string list;
    for (const SentenceId id : ids) {
        list += list.empty() ? id.str() : ", " + id.str();
    }
    return list;
}

// What the sentences at `indices` say, joined.
std::string said_at(const Transcript& transcript, std::span<const std::size_t> indices) {
    std::string said;
    for (const std::size_t index : indices) {
        said += said.empty() ? transcript.sentences[index].text : " " + transcript.sentences[index].text;
    }
    return said;
}

// --- Checks ---------------------------------------------------------------------------

// "S12's atom 2", for each atom of `answer` in the order written: how a
// repair names the atoms the ledger's checks number A1, A2, …
std::vector<std::string> positions_of(const json::Json& answer) {
    std::vector<std::string> positions;
    for (const json::Json& row : answer.at("sentences")) {
        const std::string sentence = row.at("sentence").get<std::string>();
        for (std::size_t number = 1; number <= row.at("atoms").size(); ++number) {
            positions.push_back(std::format("{}'s atom {}", sentence, number));
        }
    }
    return positions;
}

bool word_character(char character) {
    return std::isalnum(static_cast<unsigned char>(character)) != 0;
}

// `text` with `word` replaced by `by` wherever it stands as a word of its own:
// "A1" in "A1 says", never in "A10" or "MA1".
std::string replace_word(std::string_view text, std::string_view word, std::string_view by) {
    std::string replaced;
    std::size_t at = 0;
    for (std::size_t found = text.find(word); found != std::string_view::npos; found = text.find(word, at)) {
        const std::size_t end = found + word.size();
        const bool whole = (found == 0 || !word_character(text[found - 1])) &&
                           (end == text.size() || !word_character(text[end]));
        replaced += text.substr(at, found - at);
        replaced += whole ? by : word;
        at = end;
    }
    return replaced + std::string{text.substr(at)};
}

// What `finding` says, each atom it concerns ("A3") named by its position
// instead (positions_of()): the answer has no A3, only the rows it wrote.
// Only the atoms the finding names are translated, so a sentence it quotes
// that happens to say "A2" keeps its words. An atom past `positions` (one a
// recall answer was shown) keeps its number.
std::string by_position(const fidelity::Finding& finding, std::span<const std::string> positions) {
    std::string message = finding.message;
    for (const knowledge::AtomId id : finding.atoms) {
        if (id.number() >= 1 && id.number() <= positions.size()) {
            message = replace_word(message, id.str(), positions[id.number() - 1]);
        }
    }
    return message;
}

// What an answer was asked about, and what it is held to.
struct Asked {
    Transcript part{};                    // the sentences asked about, and those the held atoms cite
    std::vector<SentenceId> rows{};       // the sentences it writes rows for
    std::string said{};                   // what the prompt showed of the transcript, context too
    std::vector<std::string> speakers{};  // the video's speakers, folded
    std::vector<std::string> lexicon{};   // the video's words for things
    bool recall = false;                  // asked again: no filler, nothing written twice
    std::vector<Atom> held{};             // recall: the atoms its sentences have
};

void check_row(const json::Json& row, SentenceId sentence, const Asked& asked, Problems& problems) {
    const json::Json& filler = row.at("filler");
    if (!filler.is_null() && asked.recall) {
        problems.note(
            std::format("{} is labelled {}: a sentence asked again is never filler; give it only the "
                        "atoms it misses, or none",
                        sentence, filler.get<std::string>()));
    } else if (!filler.is_null() && !row.at("atoms").empty()) {
        problems.note(std::format("{} is labelled {} and has atoms: it is one or the other", sentence,
                                  filler.get<std::string>()));
    }
    std::size_t number = 0;
    for (const json::Json& atom : row.at("atoms")) {
        ++number;
        for (const json::Json& also : atom.at("also")) {
            const SentenceId later = sentence_of(also);
            if (later <= sentence || !std::ranges::contains(asked.rows, later)) {
                problems.note(
                    std::format("{}'s atom {} also names {}: name only later sentences among those to "
                                "read",
                                sentence, number, later));
            }
        }
    }
}

// A row for sentences asked about only, each once, in order.
void check_rows(const json::Json& answer, const Asked& asked, Problems& problems) {
    std::size_t next = 0;  // where the next row may be, among asked.rows
    for (const json::Json& row : answer.at("sentences")) {
        const SentenceId sentence = sentence_of(row.at("sentence"));
        const auto at = std::ranges::find(asked.rows, sentence);
        if (at == asked.rows.end()) {
            problems.note(std::format("{} is not among the sentences to read ({}): write rows for those only",
                                      sentence, named(asked.rows)));
            continue;
        }
        const auto index = static_cast<std::size_t>(at - asked.rows.begin());
        if (index < next) {
            problems.note(
                std::format("{} is written again, or out of order: one row a sentence, in order", sentence));
        }
        next = std::max(next, index + 1);
        check_row(row, sentence, asked, problems);
    }
}

// A speaker is one of the video's, or named where the prompt showed.
void check_speakers(const Reading& reading, const Asked& asked, std::span<const std::string> positions,
                    Problems& problems) {
    for (std::size_t index = 0; index < reading.atoms.size(); ++index) {
        const std::optional<std::string>& speaker = reading.atoms[index].speaker;
        if (speaker.has_value() && !std::ranges::contains(asked.speakers, folded(*speaker)) &&
            !says(asked.said, *speaker)) {
            problems.note(
                std::format("{} gives its speaker as \"{}\", whom neither the video's speakers nor these "
                            "sentences name: give null when the transcript does not tell",
                            positions[index], *speaker));
        }
    }
}

bool share_a_sentence(const Atom& lhs, const Atom& rhs) {
    return std::ranges::any_of(lhs.sentences,
                               [&](SentenceId id) { return std::ranges::contains(rhs.sentences, id); });
}

// A recalled atom adds to what its sentence's atoms hold, not repeats it.
void check_repeats(const Reading& added, std::span<const Atom> held, std::span<const std::string> positions,
                   Problems& problems) {
    for (std::size_t index = 0; index < added.atoms.size(); ++index) {
        const Atom& atom = added.atoms[index];
        const bool repeats = std::ranges::any_of(held, [&](const Atom& kept) {
            return share_a_sentence(atom, kept) && reason::word_distance(atom.text, kept.text) < kRepeated;
        });
        if (repeats) {
            problems.note(std::format("{} repeats an atom its sentence has: write only what its atoms miss",
                                      positions[index]));
        }
    }
}

// What is wrong with `answer` (the header comment, step 2), each said as
// what to do instead.
std::vector<std::string> answer_problems(const json::Json& answer, const Asked& asked) {
    Problems problems;
    check_rows(answer, asked, problems);
    if (!problems.empty()) {
        return std::move(problems).list();  // the ledger's checks would only say it again
    }
    const Result<Reading> reading = reading_of(answer);
    if (!reading.has_value()) {
        problems.note(reading.error().message());
        return std::move(problems).list();
    }
    // The ledger checks the answer's atoms as written, A1, A2, …; a recall
    // answer's with the atoms its sentences have, which account for them.
    knowledge::AtomLedger ledger{.atoms = reading->atoms, .fillers = reading->fillers};
    for (const Atom& kept : asked.held) {
        ledger.atoms.push_back(kept);
        ledger.atoms.back().id = knowledge::AtomId{static_cast<std::uint32_t>(ledger.atoms.size())};
    }
    const std::vector<std::string> positions = positions_of(answer);
    for (const fidelity::Finding& finding :
         fidelity::check_ledger(asked.part, ledger, asked.lexicon).findings) {
        if (finding.severity == fidelity::Severity::error) {
            problems.note(by_position(finding, positions));
        }
    }
    check_speakers(*reading, asked, positions, problems);
    if (asked.recall) {
        check_repeats(*reading, asked.held, positions, problems);
    }
    return std::move(problems).list();
}

// An answer the army accepted, read: its checks read it the same way.
Reading accepted(const agents::Answer& answer) {
    Result<Reading> reading = reading_of(answer.value);
    RR_ASSERT(reading.has_value(), "an accepted answer passed answer_problems(), which reads it");
    return std::move(*reading);
}

// --- What a window was read as -----------------------------------------------------------

// A window, and what was made of it.
struct Atomized {
    SentenceWindow window{};
    std::vector<SentenceId> ids{};
    std::size_t number = 0;            // its mission's, atomize-<number>; 0: too long to read
    int drawn = 0;                     // the samples asked of it, every wave's
    std::vector<Reading> readings{};   // the answers accepted
    std::optional<Reading> reading{};  // the one kept; none: no answer read the window
    std::vector<Stretch> unsettled{};  // where the kept reading is not settled
};

// How a reading reads in a settle prompt: a line a sentence of `ids`.
std::string reading_lines(const Reading& reading, std::span<const SentenceId> ids) {
    std::string lines;
    for (const SentenceId id : ids) {
        std::string line;
        if (const auto filler = std::ranges::find(reading.fillers, id, &knowledge::Filler::sentence);
            filler != reading.fillers.end()) {
            line = std::format("filler {}", knowledge::to_string(filler->label));
        }
        for (const Atom& atom : reading.atoms) {
            if (atom.sentences.front() != id) {
                continue;
            }
            std::string also;
            for (std::size_t index = 1; index < atom.sentences.size(); ++index) {
                also += std::format(", also {}", atom.sentences[index]);
            }
            line += std::format("{}{} ({}{}): {}", line.empty() ? "" : "; ", knowledge::to_string(atom.kind),
                                knowledge::to_string(atom.modality), also, atom.text);
        }
        if (line.empty()) {  // an accepted reading accounts for every sentence
            line = "held by an atom of an earlier sentence";
        }
        lines += std::format("{}- {}: {}", lines.empty() ? "" : "\n", id, line);
    }
    return lines;
}

// --- The models' part ---------------------------------------------------------------------

// What the models make of one transcript: each window read, voted on,
// settled and recalled (the header comment's steps 1 to 4), then joined
// (step 5). What they could not do is noted in output(), as findings.
class Atomizer {
public:
    Atomizer(const StageContext& context, const Transcript& transcript, const knowledge::Outline& outline,
             const reason::AtomizeCompute& compute, agents::Army& army, StagePrompt prompt, json::Json shared)
        : context_(&context)
        , transcript_(&transcript)
        , outline_(&outline)
        , compute_(compute)
        , army_(&army)
        , prompt_(std::move(prompt))
        , shared_(std::move(shared)) {
        for (const knowledge::Speaker& speaker : outline.brief.speakers) {
            speakers_.push_back(folded(speaker.name));
        }
        for (const knowledge::LexiconTerm& term : outline.brief.lexicon) {
            lexicon_.push_back(term.term);
        }
        // The commander settles with its own thinking budget, within half
        // of what it holds: a budget that left no room for the sentences
        // would leave nothing to think about.
        const agents::Engine* commander = engine_serving(army, "commander");
        if (commander != nullptr && commander->thinking.thinking_budget.has_value()) {
            settle_budget_ = commander->thinking.thinking_budget;
            settle_budget_->tokens = std::min(settle_budget_->tokens, commander->ctx_per_slot / 2);
        }
    }

    // Steps 1 to 3: each window read, in waves while its readings leave a
    // sentence unsettled that more could settle; its readings voted on, and
    // one kept.
    Result<std::vector<Atomized>> read() {
        RR_TRY_ASSIGN(WindowPlan plan, plan_windows(
                                           *transcript_, kWindowBytes, *army_,
                                           [this](const SentenceWindow& window, std::size_t number) {
                                               return read_mission(window, number);
                                           },
                                           kContext));
        progress(std::format("atomize: the infantry reads {} {}, {} {}", plan.missions.size(),
                             plan.missions.size() == 1 ? "window" : "windows", compute_.waves.first,
                             compute_.waves.first == 1 ? "time" : "times each"));
        std::vector<Atomized> windows;
        windows.reserve(plan.windows.size() + plan.too_long.size());
        for (std::size_t index = 0; index < plan.windows.size(); ++index) {
            const SentenceWindow& window = plan.windows[index];
            windows.push_back(Atomized{.window = window,
                                       .ids = ids_between(window.begin, window.end),
                                       .number = index + 1,  // plan_windows() numbers its missions so
                                       .drawn = compute_.waves.first});
        }
        RR_TRY_ASSIGN(const std::vector<agents::Outcome> outcomes, fight(std::move(plan.missions)));
        for (std::size_t index = 0; index < outcomes.size(); ++index) {
            take(windows[index], outcomes[index]);
        }
        RR_TRY(draw_waves(windows));
        for (Atomized& window : windows) {
            keep(window);
        }
        // Set aside, after the windows read: no answer read them, so where
        // they stand among the others changes nothing.
        for (const SentenceWindow& window : plan.too_long) {
            output_.findings.push_back(
                std::format("{} no atoms: even alone, it is more than {} holds at once",
                            sentences_have(*transcript_, window.begin, window.end), model("infantry")));
            windows.push_back(Atomized{.window = window, .ids = ids_between(window.begin, window.end)});
        }
        return windows;
    }

    // Step 3's end: the stretches no vote settled, read again by the
    // commander; or, where the tier does not ask it, said.
    Result<void> settle(std::vector<Atomized>& windows) {
        std::vector<std::pair<std::size_t, Stretch>> asked;  // the window and stretch of each mission
        std::vector<agents::Mission> missions;
        for (std::size_t index = 0; index < windows.size(); ++index) {
            for (const Stretch& stretch : windows[index].unsettled) {
                const auto [first, end] = global(windows[index], stretch);
                if (!compute_.contested_to_commander) {
                    output_.findings.push_back(
                        std::format("{} readings that disagree: the one most agreed with stands",
                                    sentences_have(*transcript_, first, end)));
                    continue;
                }
                if (engine_serving(*army_, "commander") == nullptr) {
                    output_.findings.push_back(
                        std::format("{} readings that disagree, and the pod serves no commander to settle "
                                    "them: the one most agreed with stands",
                                    sentences_have(*transcript_, first, end)));
                    continue;
                }
                RR_TRY_ASSIGN(agents::Mission mission,
                              settle_mission(windows[index], stretch, missions.size() + 1));
                if (!army_->holds(mission)) {
                    output_.findings.push_back(
                        std::format("{} readings that disagree, more than {} holds at once: the one most "
                                    "agreed with stands",
                                    sentences_have(*transcript_, first, end), model("commander")));
                    continue;
                }
                asked.emplace_back(index, stretch);
                missions.push_back(std::move(mission));
            }
        }
        if (missions.empty()) {
            return ok();
        }
        progress(std::format("atomize: the commander settles {} {} the readings disagree on", missions.size(),
                             missions.size() == 1 ? "stretch" : "stretches"));
        RR_TRY_ASSIGN(const std::vector<agents::Outcome> outcomes, fight(std::move(missions)));
        for (std::size_t at = 0; at < asked.size(); ++at) {
            settle_with(windows[asked[at].first], asked[at].second, outcomes[at]);
        }
        return ok();
    }

    // Step 4: the sentences their atoms barely hold, asked again; what the
    // answers add is added, and nothing is taken away.
    Result<void> recall(std::vector<Atomized>& windows) {
        if (!compute_.recall_pass) {
            return ok();
        }
        std::vector<std::size_t> asked;  // the window of each mission
        std::vector<agents::Mission> missions;
        for (std::size_t index = 0; index < windows.size(); ++index) {
            const std::vector<std::size_t> thin = barely_held(windows[index]);
            if (thin.empty()) {
                continue;
            }
            RR_TRY_ASSIGN(agents::Mission mission, recall_mission(windows[index], thin, missions.size() + 1));
            if (army_->holds(mission)) {  // the window was held whole: this is less, and nearly always held
                asked.push_back(index);
                missions.push_back(std::move(mission));
            }
        }
        if (missions.empty()) {
            return ok();
        }
        progress(
            std::format("atomize: the infantry reads {} {} again, where atoms hold little of what was said",
                        missions.size(), missions.size() == 1 ? "window" : "windows"));
        RR_TRY_ASSIGN(const std::vector<agents::Outcome> outcomes, fight(std::move(missions)));
        for (std::size_t at = 0; at < asked.size(); ++at) {
            if (outcomes[at].answers.empty()) {
                continue;  // nothing is lost: the atoms read stand
            }
            const Reading added = accepted(outcomes[at].answers.front());
            std::optional<Reading>& kept = windows[asked[at]].reading;
            RR_ASSERT(kept.has_value(), "only a window with a reading has sentences to ask again");
            kept->atoms.insert(kept->atoms.end(), added.atoms.begin(), added.atoms.end());
        }
        return ok();
    }

    // Step 5: the windows joined, and checked whole. A sentence no answer
    // read is unaccounted for, as a finding has said already.
    knowledge::AtomLedger finish(const std::vector<Atomized>& windows) {
        std::vector<Reading> kept;
        kept.reserve(windows.size());
        std::set<SentenceId> unread;
        for (const Atomized& window : windows) {
            if (window.reading.has_value()) {
                kept.push_back(*window.reading);
            } else {
                unread.insert(window.ids.begin(), window.ids.end());
            }
        }
        knowledge::AtomLedger ledger = ledger_of(kept);
        // Every window's answers were held to the video's lexicon: what is
        // left to find spans windows (a filler holding a word an atom of
        // another window uses), or is a window no answer read.
        for (const fidelity::Finding& finding : fidelity::check_ledger(*transcript_, ledger).findings) {
            const bool said =
                finding.check == fidelity::Check::sentence_accounted &&
                std::ranges::all_of(finding.sentences, [&](SentenceId id) { return unread.contains(id); });
            if (!said) {
                output_.findings.push_back(finding.message);
            }
        }
        return ledger;
    }

    [[nodiscard]] StageOutput& output() { return output_; }

private:
    void progress(const std::string& line) const {
        if (context_->progress) {
            context_->progress(line);
        }
    }

    // The model serving `role`, for a finding.
    [[nodiscard]] std::string model(std::string_view role) const {
        const agents::Engine* engine = engine_serving(*army_, role);
        return engine == nullptr ? std::format("the {}", role) : engine->model;
    }

    [[nodiscard]] std::vector<SentenceId> ids_between(std::size_t begin, std::size_t end) const {
        return ids_of(part_of(*transcript_, indices_of(begin, end)));
    }

    // The stretch's sentences, as indices of the transcript's.
    [[nodiscard]] static std::pair<std::size_t, std::size_t> global(const Atomized& window,
                                                                    const Stretch& stretch) {
        return {window.window.begin + stretch.begin, window.window.begin + stretch.end};
    }

    // The outline's sections that hold any of the sentences [first, end), as
    // the prompt names them: 'the section "Arches"', 'the sections "Arches"
    // and "Loads"'.
    [[nodiscard]] std::string sections_of(std::size_t first, std::size_t end) const {
        std::vector<std::string> titles;
        for (const knowledge::Section& section : outline_->sections) {
            if (index_of(section.first) < end && index_of(section.last) >= first) {
                titles.push_back(std::format("\"{}\"", section.title));
            }
        }
        RR_ASSERT(!titles.empty(), "the outline covers its transcript (read_input())");
        std::string named = titles.size() == 1 ? "the section " : "the sections ";
        for (std::size_t index = 0; index < titles.size(); ++index) {
            if (index > 0) {
                named += index + 1 == titles.size() ? " and " : ", ";
            }
            named += titles[index];
        }
        return named;
    }

    // An answer about the sentences at `indices`, whose prompt showed
    // `shown` of the transcript.
    [[nodiscard]] Asked asked_about(std::span<const std::size_t> indices,
                                    std::span<const std::size_t> shown) const {
        Asked asked{.part = part_of(*transcript_, indices),
                    .said = said_at(*transcript_, shown),
                    .speakers = speakers_,
                    .lexicon = lexicon_};
        asked.rows = ids_of(asked.part);
        return asked;
    }

    // A mission of the prompt, rendered with `order`, its seed derived from
    // its id; its answer checked as asked.
    [[nodiscard]] Result<agents::Mission> mission_of(std::string id, const json::Json& order,
                                                     int answer_tokens, std::string role, Asked asked) const {
        RR_TRY_ASSIGN(const prompt::Rendered rendered, context_->prompts->render(kPrompt, shared_, order));
        const std::int64_t seed = llm::derived_seed(context_->job->salt, AtomizeStage::kName, id, 0);
        return agents::Mission{
            .id = std::move(id),
            .kind = std::string{kPrompt},
            .prompt = prompt_.info->label(),
            .role = std::move(role),
            .request = llm::ChatRequest{.messages = {llm::Message::system(rendered.system),
                                                     llm::Message::user(rendered.user)},
                                        .seed = seed,
                                        .thinking = compute_.thinking},
            .answer_tokens = answer_tokens,
            .schema = prompt_.schema,
            .check = [asked = std::move(asked)](
                         const json::Json& answer) { return answer_problems(answer, asked); },
            .repairs = compute_.repairs};
    }

    // The order of a mission about the sentences of `window`, which it
    // shows with their context.
    [[nodiscard]] json::Json order_of(std::string_view mode, const SentenceWindow& window) const {
        return json::Json{{"mode", std::string{mode}},
                          {"sections", sections_of(window.begin, window.end)},
                          {"before", sentence_lines(*transcript_, window.context_begin, window.begin)},
                          {"sentences", sentence_lines(*transcript_, window.begin, window.end)},
                          {"after", sentence_lines(*transcript_, window.end, window.context_end)},
                          {"readings", ""},
                          {"atoms", ""}};
    }

    [[nodiscard]] Result<agents::Mission> read_mission(const SentenceWindow& window,
                                                       std::size_t number) const {
        RR_TRY_ASSIGN(agents::Mission mission,
                      mission_of(std::format("atomize-{}", number), order_of("read", window),
                                 answer_tokens(window.end - window.begin, lines_bytes(*transcript_, window)),
                                 "infantry",
                                 asked_about(indices_of(window.begin, window.end),
                                             indices_of(window.context_begin, window.context_end))));
        mission.samples = compute_.waves.first;
        // How sure each answer was, when a lone sample must be sure to count.
        mission.request.token_probabilities = compute_.waves.sure > 0;
        return mission;
    }

    // Wave `wave` of `window`: `samples` more of its reading, drawn after
    // those drawn, from its own seed on (sample j draws seed + j), as one
    // request for them all would have drawn them.
    [[nodiscard]] Result<agents::Mission> wave_mission(const Atomized& window, int wave, int samples) const {
        RR_TRY_ASSIGN(agents::Mission mission, read_mission(window.window, window.number));
        RR_ASSERT(mission.request.seed.has_value(), "every mission has a seed (mission_of())");
        mission.id = std::format("{}-wave-{}", mission.id, wave);
        mission.request.seed = *mission.request.seed + window.drawn;
        mission.samples = samples;
        return mission;
    }

    [[nodiscard]] Result<agents::Mission> settle_mission(const Atomized& window, const Stretch& stretch,
                                                         std::size_t number) const {
        const auto [first, end] = global(window, stretch);
        const SentenceWindow around = window_of(*transcript_, first, end, kContext);
        const std::span<const SentenceId> ids =
            std::span(window.ids).subspan(stretch.begin, stretch.end - stretch.begin);
        std::string readings;
        for (std::size_t reader = 0; reader < window.readings.size(); ++reader) {
            readings += std::format("{}Reader {}:\n{}", readings.empty() ? "" : "\n\n", reader + 1,
                                    reading_lines(window.readings[reader], ids));
        }
        json::Json order = order_of("settle", around);
        order["readings"] = std::move(readings);
        RR_TRY_ASSIGN(agents::Mission mission,
                      mission_of(std::format("settle-{}", number), order,
                                 answer_tokens(end - first, lines_bytes(*transcript_, around)), "commander",
                                 asked_about(indices_of(first, end),
                                             indices_of(around.context_begin, around.context_end))));
        mission.request.thinking = true;
        mission.request.thinking_budget = settle_budget_;  // none: the engine's own, whole
        return mission;
    }

    // The sentences of `window` whose atoms hold less than kLeastCoverage of
    // their words, as indices of the transcript's. A filler has no atoms,
    // and is not among them.
    [[nodiscard]] std::vector<std::size_t> barely_held(const Atomized& window) const {
        std::vector<std::size_t> thin;
        if (!window.reading.has_value()) {
            return thin;
        }
        for (const SentenceId id : window.ids) {
            std::vector<Atom> atoms;
            std::ranges::copy_if(window.reading->atoms, std::back_inserter(atoms),
                                 [&](const Atom& atom) { return std::ranges::contains(atom.sentences, id); });
            if (!atoms.empty() && coverage(transcript_->sentences[index_of(id)], atoms) < kLeastCoverage) {
                thin.push_back(index_of(id));
            }
        }
        return thin;
    }

    // The mission that asks the sentences at `thin` again, shown the atoms
    // they have. They may not follow one another, so they are shown alone,
    // without context; the atoms that hold them say what they are about.
    [[nodiscard]] Result<agents::Mission> recall_mission(const Atomized& window,
                                                         std::span<const std::size_t> thin,
                                                         std::size_t number) const {
        const std::optional<Reading>& reading = window.reading;
        RR_ASSERT(reading.has_value(), "only a window with a reading has sentences to ask again");
        Asked asked = asked_about(thin, thin);
        std::set<std::size_t> cited(thin.begin(), thin.end());  // the sentences the answer is checked on
        std::string atoms;
        for (const Atom& atom : reading->atoms) {
            const bool holds_one = std::ranges::any_of(
                atom.sentences, [&](SentenceId id) { return std::ranges::contains(asked.rows, id); });
            if (!holds_one) {
                continue;
            }
            asked.held.push_back(atom);
            atoms +=
                std::format("{}- {}: {} ({}): {}", atoms.empty() ? "" : "\n", atom.sentences.front(),
                            knowledge::to_string(atom.kind), knowledge::to_string(atom.modality), atom.text);
            for (const SentenceId id : atom.sentences) {
                cited.insert(index_of(id));
            }
        }
        asked.part = part_of(*transcript_, std::vector<std::size_t>(cited.begin(), cited.end()));
        asked.recall = true;
        std::string lines;
        for (const std::size_t index : thin) {
            lines += (lines.empty() ? "" : "\n") + sentence_lines(*transcript_, index, index + 1);
        }
        const json::Json order{{"mode", "recall"},
                               {"sections", sections_of(thin.front(), thin.back() + 1)},
                               {"before", ""},
                               {"sentences", lines},
                               {"after", ""},
                               {"readings", ""},
                               {"atoms", std::move(atoms)}};
        return mission_of(std::format("recall-{}", number), order, answer_tokens(thin.size(), lines.size()),
                          "infantry", std::move(asked));
    }

    // Takes `outcome`'s answers as more of `window`'s readings, each with how
    // sure its answer was of each sentence. A window no answer of any wave
    // read is a finding.
    void take(Atomized& window, const agents::Outcome& outcome) {
        for (const agents::Answer& answer : outcome.answers) {
            Reading reading = accepted(answer);
            reading.confidence = confidence_of(answer.choice.content, answer.choice.probabilities);
            window.readings.push_back(std::move(reading));
        }
        if (window.readings.empty()) {
            output_.findings.push_back(std::format(
                "{} no atoms: {}", sentences_have(*transcript_, window.window.begin, window.window.end),
                why_unanswered(outcome, *army_, "read them")));
        }
    }

    // The votes of `window`'s readings on its sentences.
    [[nodiscard]] std::vector<reason::Consensus> votes_of(const Atomized& window) const {
        std::vector<std::string> items;
        items.reserve(window.ids.size());
        for (const SentenceId id : window.ids) {
            items.push_back(id.str());
        }
        Result<std::vector<reason::Consensus>> votes = reason::vote_keyed(
            items, ballots_of(window.readings, window.ids), compute_.waves.quorum, compute_.waves.sure);
        RR_ASSERT(votes.has_value(), "the readings' ballots are on the window's sentences, one a sample");
        return std::move(*votes);
    }

    // Step 3's waves: `step` more samples of each window that has a reading
    // and a sentence unsettled that more could settle, up to the tier's
    // `max` (reason::next_wave()). A window no answer read gets none: the
    // same question would get no better answer.
    Result<void> draw_waves(std::vector<Atomized>& windows) {
        for (int wave = 2;; ++wave) {
            std::vector<std::size_t> asked;  // the window of each mission
            std::vector<agents::Mission> missions;
            for (std::size_t index = 0; index < windows.size(); ++index) {
                const Atomized& window = windows[index];
                if (window.readings.empty()) {
                    continue;
                }
                const int more = reason::next_wave(votes_of(window), window.drawn, compute_.waves);
                if (more > 0) {
                    RR_TRY_ASSIGN(agents::Mission mission, wave_mission(window, wave, more));
                    missions.push_back(std::move(mission));
                    asked.push_back(index);
                }
            }
            if (missions.empty()) {
                return ok();
            }
            progress(
                std::format("atomize: the infantry reads {} {} again, wave {}: their readings left a "
                            "sentence unsettled",
                            missions.size(), missions.size() == 1 ? "window" : "windows", wave));
            std::vector<int> samples;
            samples.reserve(missions.size());
            for (const agents::Mission& mission : missions) {
                samples.push_back(mission.samples);
            }
            RR_TRY_ASSIGN(const std::vector<agents::Outcome> outcomes, fight(std::move(missions)));
            for (std::size_t at = 0; at < asked.size(); ++at) {
                windows[asked[at]].drawn += samples[at];
                take(windows[asked[at]], outcomes[at]);
            }
        }
    }

    // Keeps one of `window`'s readings: the only one, or the base of their
    // vote; and says where it is not settled.
    void keep(Atomized& window) const {
        if (window.readings.empty()) {
            return;
        }
        const std::vector<reason::Consensus> votes = votes_of(window);
        const std::size_t base = base_of(window.readings, window.ids, votes);
        window.reading = window.readings[base];
        window.unsettled = unsettled(window.readings[base], window.ids, votes);
    }

    // The commander's reading of `stretch` replaces the base's there; without
    // one, the base stands, and a finding says why.
    void settle_with(Atomized& window, const Stretch& stretch, const agents::Outcome& outcome) {
        if (outcome.answers.empty()) {
            const auto [first, end] = global(window, stretch);
            output_.findings.push_back(std::format(
                "{} readings that disagree, and {}: the one most agreed with stands",
                sentences_have(*transcript_, first, end), why_unanswered(outcome, *army_, "settle them")));
            return;
        }
        std::optional<Reading>& base = window.reading;
        RR_ASSERT(base.has_value(), "only a window with a reading has stretches to settle");
        base = replaced(*base, window.ids, stretch, accepted(outcome.answers.front()));
    }

    // The outcomes of `missions`, sent as one squad. After a ceiling, the
    // result is this job's alone: a job allowed more would have asked what
    // this one could not.
    Result<std::vector<agents::Outcome>> fight(std::vector<agents::Mission> missions) {
        RR_TRY_ASSIGN(std::vector<agents::Outcome> outcomes,
                      army_->squad(std::move(missions), context_->stop));
        if (std::ranges::any_of(outcomes, [](const agents::Outcome& outcome) {
                return outcome.verdict == agents::Verdict::ceiling;
            })) {
            output_.reusable = false;
        }
        return outcomes;
    }

    const StageContext* context_;
    const Transcript* transcript_;
    const knowledge::Outline* outline_;
    reason::AtomizeCompute compute_;
    agents::Army* army_;
    StagePrompt prompt_;
    json::Json shared_;
    std::vector<std::string> speakers_;  // the video's, folded
    std::vector<std::string> lexicon_;   // the video's words for things
    std::optional<llm::ThinkingBudget> settle_budget_;
    StageOutput output_;
};

// What the prompt's system part shows: the video, and its brief.
json::Json shared_of(const Source& source, const knowledge::Brief& brief) {
    std::string speakers;
    for (const knowledge::Speaker& speaker : brief.speakers) {
        speakers += std::format("{}- {}{}", speakers.empty() ? "" : "\n", speaker.name,
                                speaker.role.has_value() ? ", " + *speaker.role : "");
    }
    std::string lexicon;
    for (const knowledge::LexiconTerm& term : brief.lexicon) {
        lexicon += std::format("{}- {}{}", lexicon.empty() ? "" : "\n", term.term,
                               term.meaning.has_value() ? ": " + *term.meaning : "");
    }
    const json::Json video = video_shared(source.video);
    return json::Json{{"title", video.at("title")},
                      {"channel", video.at("channel")},
                      {"gist", brief.gist.value_or("")},
                      {"speakers", std::move(speakers)},
                      {"lexicon", std::move(lexicon)}};
}

struct Read {
    Source source{};
    Transcript transcript{};
    knowledge::Outline outline{};
    reason::Tier tier{};
};

Result<Read> read_input(const StageContext& context) {
    RR_ASSERT(context.job != nullptr, "a stage runs for a job");
    Read read;
    RR_TRY_ASSIGN(read.source, source_from_json(context.inputs.at(std::string{SourceStage::kName})));
    RR_TRY_ASSIGN(read.transcript,
                  knowledge::transcript_from_json(context.inputs.at(std::string{TranscriptStage::kName})));
    if (read.transcript.sentences.empty()) {
        return fail(Errc::invalid_argument, "the transcript has no sentences: there is nothing to atomize");
    }
    RR_TRY_ASSIGN(read.outline,
                  knowledge::outline_from_json(context.inputs.at(std::string{OutlineStage::kName})));
    if (const std::vector<std::string> problems = knowledge::outline_problems(read.outline, read.transcript);
        !problems.empty()) {
        return fail(Errc::internal,
                    std::format("the outline does not cover its transcript: {}", problems.front()));
    }
    RR_TRY_ASSIGN(read.tier, job_tier(context));
    return read;
}

}  // namespace

Result<json::Json> AtomizeStage::fingerprint(const StageContext& context) const {
    RR_ASSERT(context.job != nullptr, "a stage runs for a job");
    RR_TRY_ASSIGN(const reason::Tier tier, job_tier(context));
    if (!context.army) {
        return fail(
            Errc::needs_user,
            "atomizing needs models, and no pod is configured: rreport doctor says how to set one up");
    }
    RR_ASSERT(context.prompts != nullptr, "a stage that asks models is given the prompts");
    RR_TRY_ASSIGN(json::Json prompts, prompts_fingerprint(*context.prompts, {kPrompt}));
    RR_TRY_ASSIGN(agents::Army* const army, context.army());
    const auto holds = [&](std::string_view role) {
        const agents::Engine* engine = engine_serving(*army, role);
        return engine == nullptr ? 0 : engine->ctx_per_slot;
    };
    return json::Json{
        {"tier", tier.canonical(reason::Stage::atomize)},
        {"window", json::Json{{"bytes", kWindowBytes}, {"context", kContext}}},
        {"least_coverage", kLeastCoverage},
        {"repeated", kRepeated},
        {"prompts", std::move(prompts)},
        {"army", army_fingerprint(*army)},
        {"holds", json::Json{{"infantry", holds("infantry")}, {"commander", holds("commander")}}}};
}

Result<StageOutput> AtomizeStage::run(const StageContext& context) const {
    RR_TRY_ASSIGN(const Read read, read_input(context));
    RR_TRY_ASSIGN(agents::Army* const army, context.army());
    RR_TRY_ASSIGN(StagePrompt prompt, stage_prompt(*context.prompts, kPrompt));
    Atomizer atomizer{context,
                      read.transcript,
                      read.outline,
                      read.tier.atomize,
                      *army,
                      std::move(prompt),
                      shared_of(read.source, read.outline.brief)};
    RR_TRY_ASSIGN(std::vector<Atomized> windows, atomizer.read());
    RR_TRY(atomizer.settle(windows));
    RR_TRY(atomizer.recall(windows));
    const knowledge::AtomLedger ledger = atomizer.finish(windows);
    StageOutput output = std::move(atomizer.output());
    output.artifact = knowledge::to_json(ledger);
    return output;
}

}  // namespace rr::pipeline
