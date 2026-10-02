// The atomize stage on a synthetic lecture: every sentence becomes atoms or a
// filler label, read in windows, three times at `max`, voted on, settled by
// the commander and asked again where its atoms hold little; with the faults
// real models make injected where a test says.
#include <rr/pipeline/atomize_stage.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/json/json.hpp>
#include <rr/knowledge/atoms.hpp>
#include <rr/knowledge/outline.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/llm/memo.hpp>
#include <rr/oracle/oracle.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/pipeline/windows.hpp>
#include <rr/reason/tier.hpp>
#include <rr/testkit/result.hpp>

#include "lecture.hpp"
#include "oracle_army.hpp"
#include "variant_tiers.hpp"

namespace rr::pipeline {
namespace {

using knowledge::Atom;
using knowledge::AtomId;
using knowledge::AtomKind;
using knowledge::AtomLedger;
using knowledge::SentenceId;
using ::testing::Contains;
using ::testing::ElementsAre;
using testing::engines;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using testing::lecture;
using testing::lecture_ledger;
using testing::lecture_outline;
using testing::lecture_texts;
using testing::ledger_of;
using ::testing::Not;
using testing::OracleArmy;
using testing::renumbered;
using testing::reworded;
using testing::source;
using testing::transcript_of;
using testing::without;
using testkit::unwrap;

// --- The lecture (lecture.hpp), its sentences all ended ---------------------------------

// The lecture with every sentence ended: however it is cut into windows, no
// thought runs on from one into the next.
knowledge::Transcript ended_lecture() {
    std::vector<std::string> texts = lecture_texts();
    texts[6] = "Then we added a second pan.";
    texts[7] = "The weight it held doubled.";
    return transcript_of(texts);
}

// --- Faults, and what was asked ------------------------------------------------------

// The row of the `number`th sentence an answer was asked about: S4's in the
// first window's.
json::Json& row(json::Json& answer, std::size_t number) {
    return answer.at("sentences").at(number - 1);
}

// The requests that opened each mission whose id starts with `prefix`
// ("atomize-", "settle-", "recall-"): not the repairs that followed.
std::vector<llm::ChatRequest> opened(const OracleArmy& rig, std::string_view prefix) {
    std::vector<llm::ChatRequest> first;
    for (const llm::ChatRequest& request : rig.requests("atomize")) {
        const bool another = first.empty() || first.back().tag.mission_id != request.tag.mission_id;
        if (request.tag.mission_id.starts_with(prefix) && another) {
            first.push_back(request);
        }
    }
    return first;
}

// Every request of the mission `mission`, in the order sent.
std::vector<llm::ChatRequest> requests_of(const OracleArmy& rig, std::string_view mission) {
    std::vector<llm::ChatRequest> found;
    for (const llm::ChatRequest& request : rig.requests("atomize")) {
        if (request.tag.mission_id == mission) {
            found.push_back(request);
        }
    }
    return found;
}

// The last message of every request of `mission`, in the order sent: its
// order, then what each repair sent back.
std::vector<std::string> sent_to(const OracleArmy& rig, std::string_view mission) {
    std::vector<std::string> messages;
    for (const llm::ChatRequest& request : rig.requests("atomize")) {
        if (request.tag.mission_id == mission) {
            messages.push_back(request.messages.back().text());
        }
    }
    return messages;
}

// The sentences a request asks rows for: the ids that open the lines of its
// <sentences> block.
std::vector<std::string> asked_in(const llm::ChatRequest& request) {
    const std::string order = request.messages.at(1).text();
    constexpr std::string_view kOpen = "<sentences>\n";
    const std::size_t begin = order.find(kOpen) + kOpen.size();
    const std::string_view lines =
        std::string_view{order}.substr(begin, order.find("\n</sentences>") - begin);
    std::vector<std::string> ids;
    for (const auto line : std::views::split(lines, '\n')) {
        const std::string_view text{line.begin(), line.end()};
        ids.emplace_back(text.substr(0, text.find(' ')));
    }
    return ids;
}

std::vector<std::string> ids(std::uint32_t first, std::uint32_t last) {
    std::vector<std::string> all;
    for (std::uint32_t number = first; number <= last; ++number) {
        all.push_back(std::format("S{}", number));
    }
    return all;
}

class AtomizeStageTest : public ::testing::Test {
protected:
    Result<StageOutput> run(RaiseArmy army, const knowledge::Transcript& transcript = lecture(),
                            const knowledge::Outline& outline = lecture_outline()) {
        StageContext context{.job = &job_,
                             .army = std::move(army),
                             .prompts = &prompts_,
                             .progress = [this](std::string_view line) { progress_.emplace_back(line); },
                             .tiers = tiers_};
        context.inputs.emplace("source", to_json(source()));
        context.inputs.emplace("transcript", knowledge::to_json(transcript));
        context.inputs.emplace("outline", knowledge::to_json(outline));
        RR_TRY_ASSIGN(context.fingerprint, stage_.fingerprint(context));
        fingerprint_ = context.fingerprint;
        return stage_.run(context);
    }

    static AtomLedger atoms_of(const StageOutput& output) {
        return unwrap(knowledge::atom_ledger_from_json(output.artifact));
    }

    void use_tier(std::string tier) { job_.tier = std::move(tier); }

    // A tier of the test's own, named `name`: `max`, changed as `change` says.
    void use_variant(std::string name, std::function<void(reason::Tier&)> change) {
        tiers_ = testing::variant_of("max", std::move(change));
        job_.tier = std::move(name);
    }

    [[nodiscard]] const json::Json& fingerprint() const { return fingerprint_; }
    // What the stage said it was doing, a line at a time, over every run.
    [[nodiscard]] const std::vector<std::string>& progress() const { return progress_; }

private:
    library::Job job_{.id = "J1", .tier = "max"};
    const prompt::Library prompts_ = unwrap(prompt::Library::embedded());
    const AtomizeStage stage_;
    FindTier tiers_;
    json::Json fingerprint_;
    std::vector<std::string> progress_;
};

// --- Every sentence accounted for ----------------------------------------------------------

TEST_F(AtomizeStageTest, EverySentenceBecomesAtomsOrAFillerLabel) {
    OracleArmy rig;
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_TRUE(output.reusable);
    EXPECT_EQ(atoms_of(output), lecture_ledger());

    // Two windows, read by the infantry without thinking, three samples
    // drawn at once; nothing to settle, nothing to ask again.
    const std::vector<llm::ChatRequest> read = opened(rig, "atomize-");
    ASSERT_EQ(read.size(), 2U);
    std::vector<std::string> asked = asked_in(read[0]);
    const std::vector<std::string> second = asked_in(read[1]);
    asked.insert(asked.end(), second.begin(), second.end());
    EXPECT_EQ(asked, ids(1, 30)) << "every sentence read once, in order";
    for (const llm::ChatRequest& request : read) {
        EXPECT_EQ(request.model, "gemma-infantry");
        EXPECT_EQ(request.n, 3);
        EXPECT_EQ(request.thinking, false);
        EXPECT_EQ(request.messages[0].text(), read[0].messages[0].text())
            << "every window shares its system message, for the engine's cache";
    }
    EXPECT_THAT(opened(rig, "settle-"), IsEmpty());
    EXPECT_THAT(opened(rig, "recall-"), IsEmpty());
    EXPECT_EQ(read[0].seed, llm::derived_seed(0, "atomize", "atomize-1", 0)) << "each mission its own";
    EXPECT_EQ(read[1].seed, llm::derived_seed(0, "atomize", "atomize-2", 0));
    EXPECT_THAT(progress(), ElementsAre("atomize: the infantry reads 2 windows, 3 times each"));

    // The system message holds the rules and the video's brief; a window's
    // order, its sections and the sentences around it.
    const std::string system = read[0].messages[0].text();
    EXPECT_THAT(system, HasSubstr("The transcript is of the video \"Weighing radium\", by Science Hour."));
    EXPECT_THAT(system, HasSubstr("What the whole video says:\nAda weighs radium, sample by sample."));
    EXPECT_THAT(system, HasSubstr("Its speakers:\n- Ada Lovelace, lecturer"));
    EXPECT_THAT(system,
                HasSubstr("Its words for things:\n- NRC\n- half-life: how long it takes to lose half"));
    const std::string first = read[0].messages[1].text();
    EXPECT_THAT(
        first,
        HasSubstr(R"(They are from the sections "Weighing radium", "The samples" and "What they mean".)"));
    EXPECT_THAT(first, Not(HasSubstr("Before them")));
    EXPECT_THAT(first, HasSubstr(std::format("After them, for context only (write no rows for these):\n{} [",
                                             second.front())));
    const std::string last = read[1].messages[1].text();
    EXPECT_THAT(last, HasSubstr("They are from the section \"What they mean\"."));
    EXPECT_THAT(last, HasSubstr("Before them, for context only (write no rows for these):\n"));
}

TEST_F(AtomizeStageTest, ATranscriptLongerThanTheInfantryHoldsIsReadInSmallerWindows) {
    OracleArmy whole;
    const AtomLedger at_once = atoms_of(unwrap(run(whole.raise(), ended_lecture())));
    EXPECT_EQ(at_once, ledger_of(ended_lecture()));
    EXPECT_EQ(opened(whole, "atomize-").size(), 2U);

    // An infantry that holds 4,096 tokens a sequence, as each of the 48 GB
    // profile's sixteen slots does: the rules, the brief and a window's
    // answer leave room for a few sentences at a time.
    OracleArmy windowed{{}, engines({.infantry_holds = 4'096})};
    const StageOutput output = unwrap(run(windowed.raise(), ended_lecture()));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_EQ(atoms_of(output), at_once) << "what each sentence says, however it is read";

    std::vector<std::vector<std::string>> windows;
    for (const llm::ChatRequest& request : opened(windowed, "atomize-")) {
        windows.push_back(asked_in(request));
    }
    EXPECT_GT(windows.size(), 2U);
    std::ranges::sort(windows, {}, [](const std::vector<std::string>& window) {
        return SentenceId::parse(window.front())->number();
    });
    std::vector<std::string> asked;
    for (const std::vector<std::string>& window : windows) {
        asked.insert(asked.end(), window.begin(), window.end());
    }
    EXPECT_EQ(asked, ids(1, 30)) << "every sentence read once, in order";
}

// --- Checks, and repairs -------------------------------------------------------------------

TEST_F(AtomizeStageTest, EveryRuleAnAnswerBreaksIsSentBackAndRepaired) {
    struct Case {
        oracle::Fault fault;
        std::string sent_back;
    };
    std::vector<Case> cases;
    cases.push_back({reworded("atomize-1", [](json::Json& answer) { answer.at("sentences").erase(3); }),
                     "S4 is not accounted for: give it one or more atoms, or a filler label"});
    cases.push_back({reworded("atomize-1",
                              [](json::Json& answer) {
                                  row(answer, 4)["atoms"][0]["text"] = "The NRC lent us 4 scales for it.";
                              }),
                     R"(S4's atom 1 says "4", which its sentences (S4) do not: they say "3")"});
    cases.push_back(
        {reworded("atomize-1",
                  [](json::Json& answer) {
                      row(answer, 9)["atoms"][0]["must_quote"] = "the most valuable thing";
                  }),
         "S9's atom 1's quote \"the most valuable thing\" is not what was said: its sentences say \"the most "
         "precious thing\""});
    cases.push_back(
        {reworded("atomize-1",
                  [](json::Json& answer) {
                      row(answer, 4)["filler"] = "decorative";
                      row(answer, 4)["atoms"] = json::Json::array();
                  }),
         "S4 is labelled decorative, yet holds the number \"3\": re-examine it, and give it atoms "
         "unless it carries nothing"});
    cases.push_back({reworded("atomize-1",
                              [](json::Json& answer) {
                                  row(answer, 6)["filler"] = "decorative";
                                  row(answer, 6)["atoms"] = json::Json::array();
                              }),
                     "S6 is labelled decorative, yet holds the term \"half-life\""});
    cases.push_back(
        {reworded("atomize-1", [](json::Json& answer) { row(answer, 7)["atoms"][0]["also"] = {"S40"}; }),
         "S7's atom 1 also names S40: name only later sentences among those to read"});
    cases.push_back(
        {reworded("atomize-1", [](json::Json& answer) { row(answer, 7)["atoms"][0]["also"] = {"S6"}; }),
         "S7's atom 1 also names S6: name only later sentences among those to read"});
    // S22 labelled, yet told by A1 and A18: each named by its row, A1 never
    // read inside A18.
    cases.push_back(
        {reworded("atomize-1",
                  [](json::Json& answer) {
                      row(answer, 2)["atoms"][0]["also"] = {"S22"};
                      row(answer, 21)["atoms"][0]["also"] = {"S22"};
                      row(answer, 22)["filler"] = "disfluency";
                      row(answer, 22)["atoms"] = json::Json::array();
                  }),
         "S22 is labelled disfluency and told by S2's atom 1, S21's atom 1: it is one or the other"});
    cases.push_back({reworded("atomize-1",
                              [](json::Json& answer) {
                                  answer.at("sentences")
                                      .push_back(json::Json{{"sentence", "S99"},
                                                            {"filler", "greeting"},
                                                            {"atoms", json::Json::array()}});
                              }),
                     "S99 is not among the sentences to read (S1\u2013S"});
    cases.push_back({reworded("atomize-1",
                              [](json::Json& answer) {
                                  const json::Json again = row(answer, 2);
                                  answer.at("sentences").insert(answer.at("sentences").begin() + 2, again);
                              }),
                     "S2 is written again, or out of order: one row a sentence, in order"});
    cases.push_back(
        {reworded("atomize-1", [](json::Json& answer) { row(answer, 1)["atoms"] = row(answer, 2)["atoms"]; }),
         "S1 is labelled greeting and has atoms: it is one or the other"});
    cases.push_back(
        {reworded("atomize-1", [](json::Json& answer) { row(answer, 2)["atoms"][0]["speaker"] = "Bob"; }),
         "S2's atom 1 gives its speaker as \"Bob\", whom neither the video's speakers nor these sentences "
         "name: give null when the transcript does not tell"});
    for (Case& test : cases) {
        SCOPED_TRACE(test.sent_back);
        OracleArmy rig{{std::move(test.fault)}};
        const StageOutput output = unwrap(run(rig.raise()));
        EXPECT_THAT(output.findings, IsEmpty());
        EXPECT_EQ(atoms_of(output), lecture_ledger()) << "repaired";
        EXPECT_THAT(sent_to(rig, "atomize-1"), Contains(HasSubstr(test.sent_back)));
    }
}

TEST_F(AtomizeStageTest, ASpeakerTheVideoOrTheSentencesShownNameIsTaken) {
    OracleArmy rig{{reworded("atomize-1", [](json::Json& answer) {
        row(answer, 2)["atoms"][0]["speaker"] = "ADA LOVELACE";  // the brief's, though the window says "Ada"
        row(answer, 9)["atoms"][0]["speaker"] = "Marie Curie";   // named by its sentence
        row(answer, 27)["atoms"][0]["speaker"] = "Pierre";       // named by S29, shown after the window
        row(answer, 4)["atoms"][0]["names"] = {"Charles Babbage"};
    })}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings,
                ElementsAre("A2 names \"Charles Babbage\", whom the transcript given does not "
                            "name: check it was said"))
        << "a name no sentence says is worth a reader's look: published, not sent back";
    const AtomLedger ledger = atoms_of(output);
    EXPECT_EQ(ledger.atoms.at(0).speaker, "ADA LOVELACE");
    EXPECT_EQ(ledger.atoms.at(5).speaker, "Marie Curie");
    EXPECT_EQ(ledger.atoms.at(23).speaker, "Pierre") << "S27's atom";
    EXPECT_EQ(rig.requests("atomize").size(), 2U) << "taken at once, not sent back";
}

// --- Votes, and what they do not settle ------------------------------------------------------

// The first sample of the first window reads S5 as a fact; the second, when
// `both`, as a plain statement.
std::vector<oracle::Fault> s5_read_otherwise(bool both) {
    std::vector<oracle::Fault> faults{
        reworded("atomize-1", [](json::Json& answer) { row(answer, 5)["atoms"][0]["kind"] = "fact"; })};
    if (both) {
        faults.push_back(reworded(
            "atomize-1", [](json::Json& answer) { row(answer, 5)["atoms"][0]["modality"] = "asserted"; }));
    }
    return faults;
}

TEST_F(AtomizeStageTest, ReadingsThatDisagreeAreVotedOnSentenceBySentence) {
    // One infantry slot: a window's three samples are three calls.
    OracleArmy rig{s5_read_otherwise(false), engines({.infantry_slots = 1})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_EQ(atoms_of(output), lecture_ledger()) << "two readings of three make S5 a hedged claim";
    EXPECT_EQ(sent_to(rig, "atomize-1").size(), 3U);
    EXPECT_THAT(opened(rig, "settle-"), IsEmpty()) << "the vote settled every sentence";
}

TEST_F(AtomizeStageTest, WhatNoVoteSettlesTheCommanderReadsAgainThinking) {
    OracleArmy rig{s5_read_otherwise(true), engines({.infantry_slots = 1})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_EQ(atoms_of(output), lecture_ledger()) << "the commander's reading of S5 replaces the first's";

    EXPECT_THAT(progress(), Contains("atomize: the commander settles 1 stretch the readings disagree on"));
    const std::vector<llm::ChatRequest> settled = opened(rig, "settle-");
    ASSERT_EQ(settled.size(), 1U) << "S5 alone: no atom of the first reading runs on from or into it";
    EXPECT_EQ(settled[0].model, "qwen-commander");
    EXPECT_EQ(settled[0].thinking, true);
    EXPECT_EQ(settled[0].thinking_budget, (llm::ThinkingBudget{.tokens = 500, .closing = "Answer now."}));
    EXPECT_EQ(asked_in(settled[0]), ids(5, 5));
    const std::string order = settled[0].messages[1].text();
    EXPECT_THAT(order, HasSubstr("They are from the section \"Weighing radium\"."));
    EXPECT_THAT(order,
                HasSubstr("Before them, for context only (write no rows for these):\nS3 [1:00] Um, so.\nS4 "
                          "[1:30] The NRC lent us 3 scales for it.\n"));
    EXPECT_THAT(order,
                HasSubstr("Other readers disagreed about these sentences. What each made of them:\n"
                          "<readings>\nReader 1:\n- S5: fact (hedged): Maybe the scales are older than "
                          "the lab.\n\nReader 2:\n- S5: claim (asserted): Maybe the scales are older than "
                          "the lab.\n\nReader 3:\n- S5: claim (hedged): Maybe the scales are older than "
                          "the lab.\n</readings>"));
}

TEST_F(AtomizeStageTest, TheCommanderIsShownWhatEachReaderMadeOfTheStretch) {
    // S1 is labelled three ways, and S7's thought, which runs on into S8,
    // read three ways: two stretches, settled one at a time.
    const auto otherwise = [](const std::string& filler, const std::string& field, const std::string& value) {
        return reworded("atomize-1", [=](json::Json& answer) {
            row(answer, 1)["filler"] = filler;
            row(answer, 7)["atoms"][0][field] = value;
        });
    };
    OracleArmy rig{{otherwise("decorative", "kind", "fact"), otherwise("disfluency", "modality", "hedged")},
                   engines({.infantry_slots = 1})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_EQ(atoms_of(output), lecture_ledger());
    EXPECT_THAT(progress(), Contains("atomize: the commander settles 2 stretches the readings disagree on"));

    const std::vector<llm::ChatRequest> settled = opened(rig, "settle-");
    ASSERT_EQ(settled.size(), 2U);
    EXPECT_EQ(asked_in(settled[0]), ids(1, 1));
    EXPECT_THAT(settled[0].messages[1].text(),
                HasSubstr("<readings>\nReader 1:\n- S1: filler decorative\n\nReader 2:\n- S1: filler "
                          "disfluency\n\nReader 3:\n- S1: filler greeting\n</readings>"));
    EXPECT_EQ(asked_in(settled[1]), ids(7, 8)) << "the thought, whole";
    const std::string run_on = "Then we added a second pan, and the weight it held doubled.";
    EXPECT_THAT(
        settled[1].messages[1].text(),
        HasSubstr(std::format("<readings>\nReader 1:\n- S7: fact (asserted, also S8): {0}\n- S8: held by "
                              "an atom of an earlier sentence\n\nReader 2:\n- S7: claim (hedged, also "
                              "S8): {0}\n- S8: held by an atom of an earlier sentence\n\nReader 3:\n- "
                              "S7: claim (asserted, also S8): {0}\n- S8: held by an atom of an earlier "
                              "sentence\n</readings>",
                              run_on)));
}

TEST_F(AtomizeStageTest, ThePodsInfantryAloneLeavesTheVoteStanding) {
    OracleArmy rig{s5_read_otherwise(true), {engines({.infantry_slots = 1})[1]}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings,
                ElementsAre("the sentence S5 (2:00 to 2:25) has readings that disagree, and the "
                            "pod serves no commander to settle them: the one most agreed with "
                            "stands"));
    EXPECT_THAT(opened(rig, "settle-"), IsEmpty());
    EXPECT_EQ(fingerprint().at("holds").at("commander"), 0);
}

TEST_F(AtomizeStageTest, TheCommanderSettlesWithinHalfOfWhatItHolds) {
    OracleArmy rig{s5_read_otherwise(true), engines({.infantry_slots = 1, .thinking_budget = 40'000})};
    unwrap(run(rig.raise()));
    EXPECT_EQ(opened(rig, "settle-").at(0).thinking_budget,
              (llm::ThinkingBudget{.tokens = 16'384, .closing = "Answer now."}))
        << "its own 40,000 tokens would leave the sentences no room";
}

TEST_F(AtomizeStageTest, WithoutTheCommanderTheReadingMostAgreedWithStands) {
    use_variant("max-alone", [](reason::Tier& tier) { tier.atomize.contested_to_commander = false; });
    OracleArmy rig{s5_read_otherwise(true), engines({.infantry_slots = 1})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, ElementsAre("the sentence S5 (2:00 to 2:25) has readings that disagree: the "
                                             "one most agreed with stands"));
    AtomLedger first = lecture_ledger();
    first.atoms[2].kind = AtomKind::fact;
    EXPECT_EQ(atoms_of(output), first) << "the first reading wins as many votes as any";
    EXPECT_THAT(opened(rig, "settle-"), IsEmpty());
}

TEST_F(AtomizeStageTest, AStretchTooLongForTheCommanderIsLeftAsTheVoteLeftIt) {
    OracleArmy rig{s5_read_otherwise(true), engines({.commander_holds = 2'048, .infantry_slots = 1})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(
        output.findings,
        ElementsAre("the sentence S5 (2:00 to 2:25) has readings that disagree, more than qwen-commander "
                    "holds at once: the one most agreed with stands"));
    EXPECT_THAT(opened(rig, "settle-"), IsEmpty());
}

TEST_F(AtomizeStageTest, AStretchTheModelsDeclineToSettleIsLeftAsTheVoteLeftIt) {
    std::vector<oracle::Fault> faults = s5_read_otherwise(true);
    faults.push_back(oracle::refusal(oracle::When{.mission = "settle-1", .calls = 0}));
    OracleArmy rig{std::move(faults), engines({.infantry_slots = 1})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(
        output.findings,
        ElementsAre("the sentence S5 (2:00 to 2:25) has readings that disagree, and the models declined "
                    "to settle them: the one most agreed with stands"));
    AtomLedger first = lecture_ledger();
    first.atoms[2].kind = AtomKind::fact;
    EXPECT_EQ(atoms_of(output), first);
}

// --- Asked again -----------------------------------------------------------------------

// Every sample of the first window says of S2 only that Ada weighs: an atom
// that holds one of its seven words.
oracle::Fault s2_thin() {
    return reworded("atomize-1",
                    [](json::Json& answer) { row(answer, 2)["atoms"][0]["text"] = "Ada weighs."; });
}

// The lecture's ledger, with S2's thin atom before its whole one.
AtomLedger recalled_ledger() {
    AtomLedger ledger = lecture_ledger();
    Atom thin = ledger.atoms[0];
    thin.text = "Ada weighs.";
    ledger.atoms.insert(ledger.atoms.begin(), std::move(thin));
    return renumbered(std::move(ledger));
}

TEST_F(AtomizeStageTest, ASentenceItsAtomsBarelyHoldIsAskedAgain) {
    // Every sample says of S2 only that Ada weighs, and of S4 half its words.
    OracleArmy rig{{reworded("atomize-1", [](json::Json& answer) {
        row(answer, 2)["atoms"][0]["text"] = "Ada weighs.";
        row(answer, 4)["atoms"][0]["text"] = "The NRC lent us.";
    })}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    AtomLedger expected = recalled_ledger();
    expected.atoms[2].text = "The NRC lent us.";
    EXPECT_EQ(atoms_of(output), expected) << "what the answer adds is added; nothing is taken away";
    EXPECT_THAT(
        progress(),
        Contains("atomize: the infantry reads 1 window again, where atoms hold little of what was said"));

    const std::vector<llm::ChatRequest> recalled = opened(rig, "recall-");
    ASSERT_EQ(recalled.size(), 1U);
    EXPECT_EQ(recalled[0].model, "gemma-infantry");
    EXPECT_EQ(recalled[0].thinking, false);
    EXPECT_EQ(asked_in(recalled[0]), ids(2, 2)) << "S4's atom holds half its words: enough";
    const std::string order = recalled[0].messages[1].text();
    EXPECT_THAT(order, HasSubstr("They are from the section \"Weighing radium\"."));
    EXPECT_THAT(order,
                HasSubstr("The atoms read from them so far hold little of what they say:\n<atoms>\n- S2: "
                          "claim (asserted): Ada weighs.\n</atoms>"));
    EXPECT_THAT(order, Not(HasSubstr("Before them"))) << "a sentence asked again is shown alone";
}

TEST_F(AtomizeStageTest, SentencesAskedAgainAreShownAloneAndNamedOneByOne) {
    // S2, S4 and S7 thin in every sample: S7's atom, which also cites S8,
    // holds only S8's word. The answer asked again first writes a row for a
    // sentence it was not asked about.
    OracleArmy rig{{reworded("atomize-1",
                             [](json::Json& answer) {
                                 row(answer, 2)["atoms"][0]["text"] = "Ada weighs.";
                                 row(answer, 4)["atoms"][0]["text"] = "Scales.";
                                 row(answer, 7)["atoms"][0]["text"] = "Doubled.";
                             }),
                    reworded("recall-1", [](json::Json& answer) {
                        answer.at("sentences")
                            .push_back(json::Json{
                                {"sentence", "S99"}, {"filler", nullptr}, {"atoms", json::Json::array()}});
                    })}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    AtomLedger expected = recalled_ledger();  // S2: thin, then whole
    Atom thin = expected.atoms[2];            // S4's
    thin.text = "Scales.";
    expected.atoms.insert(expected.atoms.begin() + 2, std::move(thin));
    expected.atoms[6].text = "Doubled.";  // S7–S8's
    Atom alone = expected.atoms[6];
    alone.sentences = {SentenceId{7}};
    alone.text = lecture().sentences[6].text;
    expected.atoms.insert(expected.atoms.begin() + 7, std::move(alone));
    EXPECT_EQ(atoms_of(output), renumbered(std::move(expected)))
        << "S7 shown alone: its new atom runs on into nothing";

    const std::vector<llm::ChatRequest> recalled = opened(rig, "recall-");
    ASSERT_EQ(recalled.size(), 1U) << "a window's sentences asked again at once";
    EXPECT_EQ(asked_in(recalled[0]), (std::vector<std::string>{"S2", "S4", "S7"}));
    EXPECT_THAT(
        recalled[0].messages[1].text(),
        HasSubstr("<sentences>\nS2 [0:30] I'm Ada, and today we weigh radium.\nS4 [1:30] The NRC lent "
                  "us 3 scales for it.\nS7 [3:00] Then we added a second pan, and the weight it "
                  "held\n</sentences>"));
    EXPECT_THAT(
        sent_to(rig, "recall-1"),
        Contains(HasSubstr("S99 is not among the sentences to read (S2, S4, S7): write rows for those "
                           "only")));
}

TEST_F(AtomizeStageTest, AnAnswerAskedAgainAddsWithoutRepeatingOrLabelling) {
    const std::vector<std::pair<oracle::Fault, std::string>> cases{
        {reworded("recall-1", [](json::Json& answer) { row(answer, 1)["atoms"][0]["text"] = "Ada weighs."; }),
         "S2's atom 1 repeats an atom its sentence has: write only what its atoms miss"},
        {reworded("recall-1",
                  [](json::Json& answer) {
                      row(answer, 1)["filler"] = "greeting";
                      row(answer, 1)["atoms"] = json::Json::array();
                  }),
         "S2 is labelled greeting: a sentence asked again is never filler; give it only the atoms it misses, "
         "or none"}};
    for (const auto& [fault, sent_back] : cases) {
        SCOPED_TRACE(sent_back);
        OracleArmy rig{{s2_thin(), fault}};
        const StageOutput output = unwrap(run(rig.raise()));
        EXPECT_THAT(output.findings, IsEmpty());
        EXPECT_EQ(atoms_of(output), recalled_ledger()) << "repaired";
        EXPECT_THAT(sent_to(rig, "recall-1"), Contains(HasSubstr(sent_back)));
    }
}

TEST_F(AtomizeStageTest, AFastTierReadsOnceAndNeverAsksAgain) {
    use_tier("fast");
    OracleArmy rig{{s2_thin()}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    AtomLedger thin = lecture_ledger();
    thin.atoms[0].text = "Ada weighs.";
    EXPECT_EQ(atoms_of(output), thin);
    for (const llm::ChatRequest& request : opened(rig, "atomize-")) {
        EXPECT_FALSE(request.n.has_value()) << "one sample";
        EXPECT_FALSE(request.token_probabilities) << "only a tier with a `sure` asks how sure";
    }
    EXPECT_THAT(opened(rig, "recall-"), IsEmpty());
}

TEST_F(AtomizeStageTest, AnAnswerMayTakeThreeHundredAndTwentyBytesASentenceBesidesItsWords) {
    OracleArmy rig{{s2_thin()}};
    unwrap(run(rig.raise()));
    const llm::ChatRequest first = opened(rig, "atomize-").at(0);
    ASSERT_EQ(asked_in(first), ids(1, 27));
    const std::size_t bytes = lines_bytes(lecture(), SentenceWindow{.begin = 0, .end = 27});
    EXPECT_EQ(first.max_tokens, static_cast<int>((((std::size_t{27} * 320) + bytes) / 3) + 128))
        << "at the estimator's three bytes a token, and 128 to spare";
    EXPECT_EQ(opened(rig, "recall-").at(0).max_tokens, 512)
        << "one sentence's answer: the least an answer takes";

    // Two hundred sentences of a word: the first window holds over a hundred.
    const std::vector<std::string> texts(200, "Yes.");
    const knowledge::Outline outline{
        .sections = {knowledge::Section{.first = SentenceId{1}, .last = SentenceId{200}, .title = "Yes"}}};
    OracleArmy terse;
    unwrap(run(terse.raise(), transcript_of(texts), outline));
    EXPECT_EQ(opened(terse, "atomize-").at(0).max_tokens, 8'192) << "the most an answer takes";
}

// --- Waves: more samples only where they could settle what is not ----------------------

// Two samples, a third where they disagree.
void two_then_a_third(reason::Tier& tier) {
    tier.atomize.waves = reason::Waves{.first = 2, .step = 1, .max = 3, .quorum = 0.66};
}

// One sample, a second where it was unsure (and a third where they disagree).
void one_where_sure(reason::Tier& tier) {
    tier.atomize.waves = reason::Waves{.first = 1, .step = 1, .max = 3, .quorum = 0.66, .sure = 0.9};
}

// The model unsure of S5's kind in the first window's first call.
oracle::Fault s5_unsure() {
    return oracle::doubt(
        oracle::When{.mission = "atomize-1"},
        [](std::string_view pointer) { return pointer == "/sentences/4/atoms/0/kind"; }, 0.5);
}

TEST_F(AtomizeStageTest, AWindowWhoseReadingsSplitIsReadAgainFromItsNextSeed) {
    use_variant("waves", two_then_a_third);
    // One infantry slot: each sample is a call of its own, and a fault hits one.
    OracleArmy rig{s5_read_otherwise(false), engines({.infantry_slots = 1})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_EQ(atoms_of(output), lecture_ledger()) << "the third reading breaks S5's tie";

    const std::vector<llm::ChatRequest> first = requests_of(rig, "atomize-1");
    ASSERT_EQ(first.size(), 2U);
    const std::vector<llm::ChatRequest> again = requests_of(rig, "atomize-1-wave-2");
    ASSERT_EQ(again.size(), 1U) << "one more, the wave's step";
    EXPECT_EQ(again[0].seed, *first[0].seed + 2) << "the sample a request for three would have drawn third";
    EXPECT_FALSE(again[0].token_probabilities) << "no lone sample needs to be sure";
    EXPECT_THAT(requests_of(rig, "atomize-2-wave-2"), IsEmpty()) << "its readings agreed";
    EXPECT_THAT(requests_of(rig, "atomize-1-wave-3"), IsEmpty()) << "two of three settled it";
    EXPECT_THAT(opened(rig, "settle-"), IsEmpty());
    EXPECT_THAT(progress(), Contains("atomize: the infantry reads 1 window again, wave 2: their readings "
                                     "left a sentence unsettled"));
}

TEST_F(AtomizeStageTest, ALoneReadingIsReadAgainWhereTheModelWasUnsure) {
    use_variant("sure", one_where_sure);
    OracleArmy rig{{s5_unsure()}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_EQ(atoms_of(output), lecture_ledger()) << "two readings that agree settle S5";

    const std::vector<llm::ChatRequest> first = requests_of(rig, "atomize-1");
    ASSERT_EQ(first.size(), 1U);
    EXPECT_TRUE(first[0].token_probabilities) << "how sure each answer was, a token at a time";
    EXPECT_FALSE(first[0].n.has_value()) << "one sample";
    const std::vector<llm::ChatRequest> again = requests_of(rig, "atomize-1-wave-2");
    ASSERT_EQ(again.size(), 1U) << "S5 was read at 0.5, under 0.9";
    EXPECT_EQ(again[0].seed, *first[0].seed + 1);
    EXPECT_THAT(requests_of(rig, "atomize-2-wave-2"), IsEmpty()) << "the oracle was sure of every sentence";
    EXPECT_THAT(opened(rig, "settle-"), IsEmpty());
}

TEST_F(AtomizeStageTest, AnUnsureReadingWithNoWaveLeftGoesToTheCommander) {
    use_variant("sure-once", [](reason::Tier& tier) {
        tier.atomize.waves = reason::Waves{.first = 1, .step = 0, .max = 1, .quorum = 0.66, .sure = 0.9};
    });
    OracleArmy rig{{s5_unsure()}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_EQ(atoms_of(output), lecture_ledger());
    EXPECT_THAT(requests_of(rig, "atomize-1-wave-2"), IsEmpty());
    const std::vector<llm::ChatRequest> settled = opened(rig, "settle-");
    ASSERT_EQ(settled.size(), 1U) << "S5, which the lone reading was unsure of";
    EXPECT_EQ(asked_in(settled[0]), ids(5, 5));
}

TEST_F(AtomizeStageTest, WavesStopAtTheTiersMaxAndTheCommanderSettlesWhatIsLeft) {
    use_variant("two-at-most", [](reason::Tier& tier) {
        tier.atomize.waves = reason::Waves{.first = 1, .step = 1, .max = 2, .quorum = 0.66, .sure = 0.9};
    });
    // The first reading unsure of S5, the second reading it otherwise: a tie.
    OracleArmy rig{{s5_unsure(), reworded("atomize-1-wave-2", [](json::Json& answer) {
                        row(answer, 5)["atoms"][0]["kind"] = "fact";
                    })}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_EQ(atoms_of(output), lecture_ledger());
    EXPECT_EQ(requests_of(rig, "atomize-1-wave-2").size(), 1U) << "the first was unsure";
    EXPECT_THAT(requests_of(rig, "atomize-1-wave-3"), IsEmpty())
        << "two is the max, though a third could break the tie";
    const std::vector<llm::ChatRequest> settled = opened(rig, "settle-");
    ASSERT_EQ(settled.size(), 1U) << "the tie, the commander's";
    EXPECT_EQ(asked_in(settled[0]), ids(5, 5));
}

TEST_F(AtomizeStageTest, AWindowNoAnswerReadIsNotReadAgain) {
    use_variant("sure", one_where_sure);
    OracleArmy rig{{oracle::refusal(oracle::When{.mission = "atomize-2", .calls = 0})}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, ElementsAre(HasSubstr("have no atoms: the models declined to read them")));
    EXPECT_THAT(requests_of(rig, "atomize-2-wave-2"), IsEmpty()) << "the same question, the same refusal";
}

// --- What no model could do ------------------------------------------------------------

TEST_F(AtomizeStageTest, AWindowTheModelsDeclineToReadLeavesItsSentencesUnaccounted) {
    OracleArmy rig{{oracle::refusal(oracle::When{.mission = "atomize-2", .calls = 0})}};
    const StageOutput output = unwrap(run(rig.raise()));
    const std::vector<std::string> second = asked_in(opened(rig, "atomize-").at(1));
    ASSERT_EQ(second, ids(28, 30));
    EXPECT_THAT(output.findings, ElementsAre("the sentences S28\u2013S30 (13:30 to 14:55) have no atoms: the "
                                             "models declined to read them"))
        << "said once, for the window, not again as unaccounted sentences";
    EXPECT_TRUE(output.reusable) << "they would decline again";
    EXPECT_EQ(atoms_of(output), without(lecture_ledger(), {28, 29, 30}));
}

TEST_F(AtomizeStageTest, AJobCeilingMakesTheResultThisJobsAlone) {
    OracleArmy rig{{}, engines({.infantry_slots = 1}), agents::Ceilings{.completion_tokens = 1}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_FALSE(output.reusable) << "a job allowed more would have read more";
    EXPECT_THAT(output.findings, Contains(HasSubstr("the job reached its completion_tokens ceiling first")));
}

TEST_F(AtomizeStageTest, ASentenceTooLongForTheInfantryEvenAloneHasNoAtoms) {
    std::vector<std::string> texts = lecture_texts();
    std::string endless = "Sample";
    for (int word = 0; word < 12'000; ++word) {
        endless += " glowed";
    }
    texts[11] = endless + ".";
    OracleArmy rig{{}, engines({.infantry_holds = 16'384})};
    const StageOutput output = unwrap(run(rig.raise(), transcript_of(texts)));
    EXPECT_THAT(output.findings,
                ElementsAre("the sentence S12 (5:30 to 5:55) has no atoms: even alone, it is "
                            "more than gemma-infantry holds at once"));
    EXPECT_EQ(atoms_of(output), without(lecture_ledger(), {12}));
}

TEST_F(AtomizeStageTest, WithoutAPodThereIsNoLedger) {
    const Result<StageOutput> output = run({});
    EXPECT_ERRC(output, Errc::needs_user);
    EXPECT_THAT(output.error().message(), HasSubstr("atomizing needs models, and no pod is configured"));
}

// --- What is refused, and the key -------------------------------------------------------------

TEST_F(AtomizeStageTest, ATranscriptWithoutSentencesIsNothingToAtomize) {
    OracleArmy rig;
    EXPECT_ERRC(run(rig.raise(), knowledge::Transcript{}), Errc::invalid_argument);
}

TEST_F(AtomizeStageTest, AnOutlineThatDoesNotCoverItsTranscriptIsABug) {
    OracleArmy rig;
    knowledge::Outline outline = lecture_outline();
    outline.sections.pop_back();
    EXPECT_ERRC(run(rig.raise(), lecture(), outline), Errc::internal);
}

TEST_F(AtomizeStageTest, ATierRReportDoesNotShipIsNotFound) {
    use_tier("turbo");
    EXPECT_ERRC(run({}), Errc::not_found);
}

TEST_F(AtomizeStageTest, TheKeyHoldsTheTierThePromptTheArmyAndWhatEachRoleHolds) {
    OracleArmy rig;
    unwrap(run(rig.raise()));
    const json::Json max = fingerprint();
    EXPECT_EQ(max.at("window"), (json::Json{{"bytes", 2'500}, {"context", 2}}));
    EXPECT_EQ(max.at("least_coverage"), 0.5);
    EXPECT_EQ(max.at("repeated"), 0.25);
    EXPECT_EQ(max.at("prompts").at("atomize").at("label"), "atomize@1");
    EXPECT_EQ(max.at("prompts").at("atomize").at("hash"),
              unwrap(unwrap(prompt::Library::embedded()).content_hash("atomize")));
    EXPECT_EQ(max.at("holds"), (json::Json{{"infantry", 32'768}, {"commander", 32'768}}));
    EXPECT_EQ(max.at("army").size(), 2U);

    use_tier("fast");
    OracleArmy fast;
    unwrap(run(fast.raise()));
    EXPECT_NE(fingerprint().at("tier"), max.at("tier"));
    EXPECT_EQ(fingerprint().at("prompts"), max.at("prompts"));

    OracleArmy smaller{{}, engines({.infantry_holds = 16'384})};
    unwrap(run(smaller.raise()));
    EXPECT_EQ(fingerprint().at("holds").at("infantry"), 16'384);
}

}  // namespace
}  // namespace rr::pipeline
