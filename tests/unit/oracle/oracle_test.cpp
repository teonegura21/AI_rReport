#include <rr/oracle/oracle.hpp>

#include <format>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/agents/army.hpp>
#include <rr/core/clock.hpp>
#include <rr/json/json.hpp>
#include <rr/oracle/answers.hpp>
#include <rr/testkit/result.hpp>

namespace rr::oracle {
namespace {

using ::testing::HasSubstr;
using testkit::unwrap;

llm::ChatRequest asked(std::string kind, std::string mission = "m1", std::string model = "gemma") {
    llm::ChatRequest request{.model = std::move(model),
                             .messages = {llm::Message::user("S1. Water boils at 100 °C.")}};
    request.tag = llm::CallTag{.purpose = std::move(kind), .job_id = "J1", .mission_id = std::move(mission)};
    return request;
}

llm::Choice answer(std::string content) {
    return llm::Choice{.content = std::move(content), .finish = llm::FinishReason::stop};
}

// --- The oracle ------------------------------------------------------------------

TEST(Oracle, AnswersTheKindsItWasTaught) {
    Oracle oracle;
    oracle.teach("label",
                 [](const llm::ChatRequest&) -> Result<std::string> { return R"({"label": "claim"})"; });

    llm::ChatRequest three = asked("label");
    three.n = 3;
    const llm::ChatResponse response = unwrap(oracle.chat(three, {}, {}));
    ASSERT_EQ(response.choices.size(), 3U) << "one right answer, as many times as asked";
    EXPECT_EQ(response.choices[2].content, R"({"label": "claim"})");
    EXPECT_EQ(response.choices[2].finish, llm::FinishReason::stop);
    EXPECT_EQ(response.usage.prompt_tokens, 7) << "four characters a token, rounded up";
    EXPECT_EQ(response.usage.completion_tokens, 3 * 5);

    const Result<llm::ChatResponse> unknown = oracle.chat(asked("outline"), {}, {});
    EXPECT_ERRC(unknown, Errc::unsupported) << "the oracle never guesses";
    EXPECT_THAT(unknown.error().message(), HasSubstr("'outline'"));

    const std::stop_source stopped;
    stopped.request_stop();
    EXPECT_ERRC(oracle.chat(asked("label"), {}, stopped.get_token()), Errc::cancelled);
}

// --- Each fault ------------------------------------------------------------------

// A punctuate mission's request for `excerpt`, as the prompt renders it.
llm::ChatRequest punctuate(std::string_view excerpt) {
    llm::ChatRequest request = asked("punctuate");
    request.messages = {
        llm::Message::system("You restore the punctuation."),
        llm::Message::user(std::format("The excerpt:\n<excerpt>\n{}\n</excerpt>\n", excerpt))};
    return request;
}

TEST(OracleAnswers, PunctuateFromTheScriptTheCaptionsWereMadeFrom) {
    Oracle oracle;
    teach_stages(oracle, "So, Marie Curie won two Nobel prizes. She called it radium!");
    const llm::ChatResponse answer = unwrap(oracle.chat(punctuate("marie curie won two nobel"), {}, {}));
    EXPECT_EQ(answer.first().content, R"({"text":"Marie Curie won two Nobel"})")
        << "the script's text for them";
    llm::ChatRequest repair = punctuate("she called it radium");
    repair.messages.push_back(llm::Message::assistant(R"({"text":"she called it radium"})"));
    repair.messages.push_back(llm::Message::user("Your answer cannot be used."));
    EXPECT_EQ(unwrap(oracle.chat(repair, {}, {})).first().content, R"({"text":"She called it radium!"})")
        << "a repair is answered from the order's excerpt";
    EXPECT_ERRC(oracle.chat(punctuate("pierre curie"), {}, {}), Errc::invalid_argument) << "never a guess";
}

TEST(OracleAnswers, WithoutAScriptASentenceEndsEveryTwelveWords) {
    Oracle oracle;
    teach_stages(oracle);
    const std::string excerpt = "a b c d e f g h i j k l m n";
    EXPECT_EQ(unwrap(oracle.chat(punctuate(excerpt), {}, {})).first().content,
              R"({"text":"A b c d e f g h i j k l. M n."})");
    EXPECT_ERRC(oracle.chat(asked("punctuate"), {}, {}), Errc::invalid_argument) << "no excerpt to punctuate";
}

// An annotate mission's request for `sentences`, as its prompt renders them,
// with the other reader's notes when there are some.
llm::ChatRequest annotating(std::string kind, std::string_view sentences, std::string_view found = "") {
    llm::ChatRequest request = asked(std::move(kind));
    std::string task = std::format("The sentences to annotate:\n<sentences>\n{}\n</sentences>\n", sentences);
    if (!found.empty()) {
        task += std::format("\nThe other reader's notes on them:\n<found>\n{}\n</found>\n", found);
    }
    request.messages = {llm::Message::system("You read part of a video's transcript."),
                        llm::Message::user(task)};
    return request;
}

TEST(OracleAnswers, AnnotateNotesEachSentenceOfFiveWordsOrMore) {
    Oracle oracle;
    teach_stages(oracle);
    const std::string_view sentences =
        "S1 [0:00] Welcome back.\nS2 [0:02] Marie Curie measured 7 samples in 1902.\nS3 [0:06] The samples "
        "glowed in the dark.\nS4 [0:09] That took months.";
    const json::Json expected = unwrap(json::parse(R"({"facts": [
        {"sentences": ["S2"], "quote": "Marie Curie measured 7 samples in 1902.",
         "claim": "Marie Curie measured 7 samples in 1902.", "importance": "core"},
        {"sentences": ["S3"], "quote": "The samples glowed in the dark.",
         "claim": "The samples glowed in the dark.", "importance": "detail"}],
        "must_quotes": [], "terms": []})"));
    EXPECT_EQ(
        unwrap(json::parse(unwrap(oracle.chat(annotating("annotate", sentences), {}, {})).first().content)),
        expected)
        << "each sentence of five words or more, whole, core when it holds a number";

    llm::ChatRequest repair = annotating("annotate", sentences);
    repair.messages.push_back(llm::Message::assistant(R"({"facts": []})"));
    repair.messages.push_back(llm::Message::user("Your answer cannot be used."));
    EXPECT_EQ(unwrap(json::parse(unwrap(oracle.chat(repair, {}, {})).first().content)), expected)
        << "a repair is answered from the order's sentences";
    EXPECT_ERRC(oracle.chat(asked("annotate"), {}, {}), Errc::invalid_argument) << "no sentences to annotate";
}

TEST(OracleAnswers, AnnotateMoreAddsTheSentencesTheNotesDoNotCite) {
    Oracle oracle;
    teach_stages(oracle);
    const llm::ChatRequest request = annotating("annotate_more",
                                                "S2 [0:02] Marie Curie measured 7 samples in 1902.\n"
                                                "S3 [0:06] The samples glowed in the dark.\n"
                                                "S4 [0:09] That took months.\nS5 [0:11] Thank you.\n"
                                                "S12 [0:30] It glowed for years.",
                                                "- [S1, S2] Marie Curie measured seven samples.\n"
                                                "- [S3, S7] They glowed.\n- [S120] Something later.");
    EXPECT_EQ(unwrap(json::parse(unwrap(oracle.chat(request, {}, {})).first().content)),
              unwrap(json::parse(R"({"facts": [
        {"sentences": ["S4"], "quote": "That took months.", "claim": "That took months.", "importance": "detail"},
        {"sentences": ["S12"], "quote": "It glowed for years.", "claim": "It glowed for years.",
         "importance": "detail"}],
        "must_quotes": [], "terms": []})")))
        << "three words or more, cited by no note, last or not in its list: S12 is not S120";
    EXPECT_ERRC(oracle.chat(annotating("annotate_more", "S3 [0:06] That took months."), {}, {}),
                Errc::invalid_argument)
        << "no notes to add to";
}

// A request of `kind` whose last message holds `block` between <tag> and </tag>.
llm::ChatRequest holding(std::string kind, std::string_view tag, std::string_view block) {
    llm::ChatRequest request = asked(std::move(kind));
    request.messages = {llm::Message::system("You read a video's transcript."),
                        llm::Message::user(std::format("The lines:\n<{0}>\n{1}\n</{0}>\n", tag, block))};
    return request;
}

TEST(OracleAnswers, OutlineStartsASectionAtS1AndWhereTheTalkTurns) {
    Oracle oracle;
    teach_stages(oracle);
    const std::string_view sentences =
        "S1 [0:00] Welcome back to the shed.\nS2 [0:04] Today we weigh radium.\nS3 [1:10] Now, the scales "
        "we weigh it with.\nS4 [1:15] They came from Paris.\nS5 [2:30] Next, the radium itself.";
    EXPECT_EQ(unwrap(json::parse(
                  unwrap(oracle.chat(holding("outline", "sentences", sentences), {}, {})).first().content)),
              unwrap(json::parse(R"({"sections": [
        {"first": "S1", "title": "Welcome back to the shed"},
        {"first": "S3", "title": "Now, the scales we weigh"},
        {"first": "S5", "title": "Next, the radium itself"}]})")))
        << "each titled by its first five words, without the full stop";
    EXPECT_EQ(unwrap(oracle.chat(holding("outline", "sentences", "S7 [3:00] They glowed."), {}, {}))
                  .first()
                  .content,
              R"({"sections":[]})")
        << "a window where the talk does not turn starts no section";
    EXPECT_ERRC(oracle.chat(asked("outline"), {}, {}), Errc::invalid_argument) << "no sentences to outline";
}

TEST(OracleAnswers, DescribeNamesWhoSaysImListsTheAcronymsAndGistsTheFirstSentence) {
    Oracle oracle;
    teach_stages(oracle);
    const std::string_view sentences =
        "S3 [1:10] Now, the scales from NASA.\nS4 [1:15] I'm Ada, and I weigh it.\nS5 [1:20] NASA lent "
        "them, and the DNA lab too.";
    EXPECT_EQ(unwrap(json::parse(
                  unwrap(oracle.chat(holding("describe", "sentences", sentences), {}, {})).first().content)),
              unwrap(json::parse(R"({"speakers": [{"name": "Ada", "role": null}],
        "lexicon": [{"term": "NASA", "meaning": null}, {"term": "DNA", "meaning": null}],
        "gist": "Now, the scales from NASA.", "genre": "talk"})")))
        << "each acronym once";
    EXPECT_ERRC(oracle.chat(holding("describe", "sentences", "no lines here"), {}, {}),
                Errc::invalid_argument)
        << "no sentence to describe";
}

TEST(OracleAnswers, BriefSaysHowManySectionsFromWhichToWhich) {
    Oracle oracle;
    teach_stages(oracle);
    const std::string_view sections =
        "1. [0:00] Welcome back: Ada welcomes the viewers.\n2. [1:10] Now, the scales\n3. [2:30] The radium: "
        "it glows.";
    EXPECT_EQ(unwrap(oracle.chat(holding("brief", "sections", sections), {}, {})).first().content,
              R"({"gist":"A video in 3 sections, from Welcome back to The radium."})");
    EXPECT_ERRC(oracle.chat(asked("brief"), {}, {}), Errc::invalid_argument) << "no sections to sum up";
}

// An atom as the oracle writes it: the sentence itself, main and medium.
std::string oracle_atom(std::string_view text, std::string_view kind, std::string_view modality,
                        std::string_view also = "", std::string_view names = "", std::string_view terms = "",
                        std::string_view quote = "null") {
    return std::format(R"({{"also": [{}], "names": [{}], "terms": [{}], "text": "{}", "kind": "{}",
        "modality": "{}", "register": "main", "intensity": "medium", "speaker": null,
        "attributed_to": null, "must_quote": {}}})",
                       also, names, terms, text, kind, modality, quote);
}

TEST(OracleAnswers, AtomizeLabelsFillerAndMakesEveryOtherSentenceAnAtom) {
    Oracle oracle;
    teach_stages(oracle);
    const std::string_view sentences =
        "S1 [0:00] Um, so.\n"
        "S2 [0:02] Hello and welcome back.\n"
        "S3 [0:05] I'm Ada Lovelace, and NASA lent us 3 scales.\n"
        "S4 [0:10] Maybe the 'proof load' is higher.\n"
        "S5 [0:15] Then we added an arch, and the load it held\n"
        "S6 [0:19] doubled.\n"
        "S7 [0:22] Why did it double?\n"
        "S8 [0:25] And then it";
    const std::string expected = std::format(
        R"({{"sentences": [
        {{"sentence": "S1", "filler": "disfluency", "atoms": []}},
        {{"sentence": "S2", "filler": "greeting", "atoms": []}},
        {{"sentence": "S3", "filler": null, "atoms": [{}]}},
        {{"sentence": "S4", "filler": null, "atoms": [{}]}},
        {{"sentence": "S5", "filler": null, "atoms": [{}]}},
        {{"sentence": "S6", "filler": null, "atoms": []}},
        {{"sentence": "S7", "filler": null, "atoms": [{}]}},
        {{"sentence": "S8", "filler": null, "atoms": [{}]}}]}})",
        oracle_atom("I'm Ada Lovelace, and NASA lent us 3 scales.", "number", "asserted", "",
                    R"("Ada Lovelace")", R"("NASA")"),
        oracle_atom("Maybe the 'proof load' is higher.", "claim", "hedged"),
        oracle_atom("Then we added an arch, and the load it held doubled.", "claim", "asserted", R"("S6")"),
        oracle_atom("Why did it double?", "question", "question"),
        oracle_atom("And then it", "claim", "asserted"));
    EXPECT_EQ(unwrap(json::parse(
                  unwrap(oracle.chat(holding("atomize", "sentences", sentences), {}, {})).first().content)),
              unwrap(json::parse(expected)))
        << "S5 runs on into S6, which has no atoms of its own; S8 runs on past the sentences to read";

    // Closing quotes end a sentence too; a thought runs on only into the
    // next sentence; greeting words greet only with a greeting; a comma ends
    // a name.
    const std::string_view more =
        "S9 [0:30] She said \"never again.\"\nS10 [0:35] You and the show.\nS12 [0:40] Then Ada, Grace and "
        "Linus met, and the weight it held\nS14 [0:45] doubled.";
    const std::string rest = std::format(
        R"({{"sentences": [
        {{"sentence": "S9", "filler": null, "atoms": [{}]}},
        {{"sentence": "S10", "filler": null, "atoms": [{}]}},
        {{"sentence": "S12", "filler": null, "atoms": [{}]}},
        {{"sentence": "S14", "filler": null, "atoms": [{}]}}]}})",
        oracle_atom(R"(She said \"never again.\")", "claim", "asserted", "", "", "", R"("never again.")"),
        oracle_atom("You and the show.", "claim", "asserted"),
        oracle_atom("Then Ada, Grace and Linus met, and the weight it held", "claim", "asserted", "",
                    R"("Ada", "Grace", "Linus")"),
        oracle_atom("doubled.", "claim", "asserted"));
    EXPECT_EQ(unwrap(json::parse(
                  unwrap(oracle.chat(holding("atomize", "sentences", more), {}, {})).first().content)),
              unwrap(json::parse(rest)))
        << "S12 does not run on into S14, which is not the sentence after it";
    EXPECT_ERRC(oracle.chat(asked("atomize"), {}, {}), Errc::invalid_argument) << "no sentences to read";
}

TEST(OracleAnswers, AtomizeAskedAgainWritesOnlyTheAtomsTheSentencesMiss) {
    Oracle oracle;
    teach_stages(oracle);
    llm::ChatRequest request = asked("atomize");
    request.messages = {
        llm::Message::system("You read a video's transcript."),
        llm::Message::user(
            "The sentences to read:\n<sentences>\nS3 [0:05] Radium glows for years, and nobody "
            "knew why.\nS4 [0:10] It glows.\nS5 [0:12] Um, so.\n</sentences>\n\nThe atoms read "
            "from them so far:\n<atoms>\n- S3: claim (asserted): Radium glows.\n- S4: claim "
            "(asserted): It glows.\n</atoms>\n")};
    const std::string expected = std::format(
        R"({{"sentences": [
        {{"sentence": "S3", "filler": null, "atoms": [{}]}},
        {{"sentence": "S4", "filler": null, "atoms": []}},
        {{"sentence": "S5", "filler": null, "atoms": [{}]}}]}})",
        oracle_atom("Radium glows for years, and nobody knew why.", "claim", "asserted"),
        oracle_atom("Um, so.", "claim", "asserted"));
    EXPECT_EQ(unwrap(json::parse(unwrap(oracle.chat(request, {}, {})).first().content)),
              unwrap(json::parse(expected)))
        << "S4's atom holds it already; a sentence asked again is never filler";
}

// A plan mission's request for the section "The samples", its atoms `atoms`.
llm::ChatRequest planning(std::string_view atoms) {
    llm::ChatRequest request = asked("plan");
    request.messages = {llm::Message::system("You plan how one section of a video is retold."),
                        llm::Message::user(std::format(
                            "Plan section 2, \"The samples\", from 5:00 to 9:55\n\nIts atoms, in the "
                            "order said:\n<atoms>\n{}\n</atoms>\n",
                            atoms))};
    return request;
}

TEST(OracleAnswers, PlanTellsThreeAtomsAParagraphAndDeclaresAnAtomShownAgain) {
    Oracle oracle;
    teach_stages(oracle);
    const std::string_view atoms =
        "A7 [5:00] number: Sample 7 glowed.\nA8 [5:30] claim: It glowed faintly.\nA9 [6:00] claim, hedged: "
        "Maybe it is old.\nA10 [6:30] claim: It glowed faintly.\nA11 [7:00] number: Sample 8 glowed.\nA12 "
        "[7:30] claim, said by Ada: It glowed faintly.";
    EXPECT_EQ(unwrap(json::parse(unwrap(oracle.chat(planning(atoms), {}, {})).first().content)),
              unwrap(json::parse(R"({"paragraphs": [
        {"purpose": "Tells A7 to A9.", "atoms": ["A7", "A8", "A9"]},
        {"purpose": "Tells A11 and A12.", "atoms": ["A11", "A12"]}],
        "duplicates": [{"because": "It says again what A8 says.", "atom": "A10", "of": "A8"}],
        "heading": "The samples"})")))
        << "A12 says A8's words, but Ada says it: told";
    EXPECT_EQ(
        unwrap(json::parse(
            unwrap(oracle.chat(planning("A7 [5:00] number: Sample 7 glowed."), {}, {})).first().content)),
        unwrap(json::parse(R"({"paragraphs": [{"purpose": "Tells A7.", "atoms": ["A7"]}],
        "duplicates": [], "heading": "The samples"})")));
    EXPECT_ERRC(oracle.chat(holding("plan", "atoms", "A1 [0:00] claim: One."), {}, {}),
                Errc::invalid_argument)
        << "no section named";
    EXPECT_ERRC(oracle.chat(asked("plan"), {}, {}), Errc::invalid_argument) << "no atoms";
}

TEST(OracleAnswers, WriteTellsEachParagraphsAtomsEachWithItsMarker) {
    Oracle oracle;
    teach_stages(oracle);
    const std::string_view paragraphs =
        "Paragraph 1: The first bridge failed.\n"
        "A7 [1:02] number: The first bridge failed at four tons. Terms: tons.\n"
        "A8 [1:03] opinion, aside, high, said by Ada: Ada was surprised. Names: Ada.\n"
        "Paragraph 2: Tell its atoms in the order given.\n"
        "A9 [1:09] definition: Its most is its proof load. To quote: \"We call the most a bridge carries "
        "its proof load.\". Terms: proof load.\n"
        "A10 [1:14] claim: They called it \"the arch\" then. To quote: \"the arch\".";
    EXPECT_EQ(unwrap(oracle.chat(holding("write", "paragraphs", paragraphs), {}, {})).first().content,
              "The first bridge failed at four tons. [A7] Ada was surprised. [A8]\n\n"
              "Its most is its proof load. (\"We call the most a bridge carries its proof load.\") [A9] They "
              "called it \"the arch\" then. [A10]")
        << "words to quote added where its words do not hold them";
    EXPECT_ERRC(oracle.chat(holding("write", "paragraphs", "A1 [0:00] claim: One."), {}, {}),
                Errc::invalid_argument)
        << "an atom before any paragraph";
    EXPECT_ERRC(oracle.chat(holding("write", "paragraphs", ""), {}, {}), Errc::invalid_argument)
        << "no paragraph";
    EXPECT_ERRC(oracle.chat(asked("write"), {}, {}), Errc::invalid_argument) << "no paragraphs block";
}

TEST(OracleAnswers, FuseWritesTheParagraphsAsWriteDoesWhateverTheDraftsSay) {
    Oracle oracle;
    teach_stages(oracle);
    const std::string_view paragraphs =
        "Paragraph 1: The first bridge failed.\n"
        "A7 [1:02] number: The first bridge failed at four tons. Terms: tons.";
    const auto fusing = [&](std::string_view drafts) {
        llm::ChatRequest request = holding("fuse", "paragraphs", paragraphs);
        request.messages.back() =
            llm::Message::user(std::format("{}\nIts drafts:\n{}", request.messages.back().text(), drafts));
        return request;
    };
    for (const std::string_view drafts :
         {"Draft 1:\n<draft>\nAt four tons, the first bridge gave way. [A7]\n</draft>",
          "Draft 1:\n<draft>\nThe bridge collapsed spectacularly. [A7]\n</draft>\nDraft 2:\n<draft>\nIt "
          "failed. [A7]\n</draft>"}) {
        EXPECT_EQ(unwrap(oracle.chat(fusing(drafts), {}, {})).first().content,
                  "The first bridge failed at four tons. [A7]")
            << "the right draft, its drafts unread: " << drafts;
    }
    EXPECT_ERRC(oracle.chat(asked("fuse"), {}, {}), Errc::invalid_argument) << "no paragraphs block";
}

// A request whose last message holds two blocks: `first` and `second`.
llm::ChatRequest holding_both(std::string kind, std::string_view first_tag, std::string_view first,
                              std::string_view second_tag, std::string_view second) {
    llm::ChatRequest request = asked(std::move(kind));
    request.messages = {llm::Message::system("You judge a draft."),
                        llm::Message::user(std::format("<{0}>\n{1}\n</{0}>\n\n<{2}>\n{3}\n</{2}>\n",
                                                       first_tag, first, second_tag, second))};
    return request;
}

TEST(OracleAnswers, JudgeReadsEverySentenceAsSayingWhatItsAtomsSay) {
    Oracle oracle;
    teach_stages(oracle);
    const std::string_view atoms =
        "A7 [1:02] number: The bridge failed at four tons. Terms: tons.\n"
        "A8 [1:03] opinion, hedged, said by Ada: Ada thinks it was young pine. Names: Ada.";
    const std::string_view sentences =
        "1. The bridge failed at four tons. [A7]\n"
        "2. Ada thinks it was young pine, an aside. [A8][A7]";
    const llm::ChatResponse response =
        unwrap(oracle.chat(holding_both("judge", "atoms", atoms, "sentences", sentences), {}, {}));
    const json::Json judged = unwrap(json::parse(response.first().content));
    EXPECT_EQ(judged, (json::Json{{"sentences",
                                   {{{"sentence", 1},
                                     {"evidence", "A7: \"The bridge failed at four tons.\""},
                                     {"reading", nullptr},
                                     {"issue", "none"}},
                                    {{"sentence", 2},
                                     {"evidence",
                                      "A8: \"Ada thinks it was young pine.\"; A7: \"The bridge failed at "
                                      "four tons.\""},
                                     {"reading", nullptr},
                                     {"issue", "none"}}}}}));
    EXPECT_ERRC(oracle.chat(holding_both("judge", "atoms", "no atom here", "sentences", sentences), {}, {}),
                Errc::invalid_argument);
    EXPECT_ERRC(
        oracle.chat(holding_both("judge", "atoms", atoms, "sentences", "a sentence unnumbered"), {}, {}),
        Errc::invalid_argument);
    EXPECT_ERRC(
        oracle.chat(holding_both("judge", "atoms", atoms, "sentences", "First. The bridge failed. [A7]"), {},
                    {}),
        Errc::invalid_argument)
        << "a sentence that does not open with its number";
}

TEST(OracleAnswers, FlowPrefersTheShorterDraftInEitherOrder) {
    Oracle oracle;
    teach_stages(oracle);
    const auto better = [&](std::string_view first, std::string_view second) {
        const llm::ChatResponse response =
            unwrap(oracle.chat(holding_both("flow", "first", first, "second", second), {}, {}));
        const json::Json answer = unwrap(json::parse(response.first().content));
        return answer.at("better").get<std::string>();
    };
    EXPECT_EQ(better("Short.", "Longer one."), "first");
    EXPECT_EQ(better("Longer one.", "Short."), "second");
    EXPECT_EQ(better("Same.", "Also."), "tie");
}

// An eval_fact mission: its fact, as prompts/eval_fact/task.md.inja lays it
// out, and `sentences` in its block.
llm::ChatRequest fact_asked(std::string_view quote, std::string_view sentences) {
    llm::ChatRequest request = asked("eval_fact");
    request.messages = {
        llm::Message::system("You check whether an article tells one fact."),
        llm::Message::user(std::format("Does the article tell this fact?\n\nThe fact: It failed.\n"
                                       "Said in the video: \"{}\"\n<sentences>\n{}\n</sentences>\n",
                                       quote, sentences))};
    return request;
}

TEST(OracleAnswers, EvalFactIsToldWhereASentenceHoldsTheQuotesWordsInARow) {
    Oracle oracle;
    teach_stages(oracle);
    const auto read = [&](std::string_view quote, std::string_view sentences) {
        return unwrap(json::parse(unwrap(oracle.chat(fact_asked(quote, sentences), {}, {})).first().content));
    };
    const std::string_view sentences =
        "1. Arches spread the load.\n"
        "2. The First Bridge failed, at four tons, said Ada.";
    EXPECT_EQ(read("the first bridge failed at four tons", sentences),
              (json::Json{{"evidence", "2: \"The First Bridge failed, at four tons, said Ada.\""},
                          {"verdict", "told"}}))
        << "case and punctuation aside";
    EXPECT_EQ(read("the bridge failed at four tons", sentences),
              (json::Json{{"evidence", ""}, {"verdict", "not_told"}}))
        << "its words, but not in a row";
    EXPECT_ERRC(oracle.chat(holding("eval_fact", "sentences", sentences), {}, {}), Errc::invalid_argument)
        << "no quote line";
    llm::ChatRequest unclosed = fact_asked("x", sentences);
    unclosed.messages.back() = llm::Message::user(
        std::format("Said in the video: \"the first bridge\n<sentences>\n{}\n</sentences>\n", sentences));
    EXPECT_ERRC(oracle.chat(unclosed, {}, {}), Errc::invalid_argument) << "a quote never closed";
    EXPECT_ERRC(oracle.chat(fact_asked("told", "First, a sentence unnumbered."), {}, {}),
                Errc::invalid_argument);
}

TEST(OracleAnswers, EvalSupportReadsEverySentenceAsNoneQuotingItsFirstEvidence) {
    Oracle oracle;
    teach_stages(oracle);
    const std::string_view sentences =
        "1. The first bridge failed at four tons.\n"
        "   S12 [1:02, Ada] The first one we tried failed at four tons.\n"
        "   S13 [1:05] It held for an hour.\n"
        "2. Arches help.\n"
        "   (no sentence of the video: it cites none)";
    const json::Json judged = unwrap(json::parse(
        unwrap(oracle.chat(holding("eval_support", "sentences", sentences), {}, {})).first().content));
    EXPECT_EQ(judged,
              (json::Json{{"sentences",
                           {{{"sentence", 1},
                             {"evidence", "S12: \"The first one we tried failed at four tons.\""},
                             {"reading", nullptr},
                             {"issue", "none"}},
                            {{"sentence", 2}, {"evidence", ""}, {"reading", nullptr}, {"issue", "none"}}}}}))
        << "the speaker in the brackets, never in the quote";
    EXPECT_ERRC(oracle.chat(holding("eval_support", "sentences", "First. Unnumbered."), {}, {}),
                Errc::invalid_argument);
}

TEST(OracleAnswers, EvalFlowPrefersTheShorterVersionInEitherOrder) {
    Oracle oracle;
    teach_stages(oracle);
    const auto better = [&](std::string_view first, std::string_view second) {
        const llm::ChatResponse response =
            unwrap(oracle.chat(holding_both("eval_flow", "first", first, "second", second), {}, {}));
        return unwrap(json::parse(response.first().content)).at("better").get<std::string>();
    };
    EXPECT_EQ(better("Short.", "Longer one."), "first");
    EXPECT_EQ(better("Longer one.", "Short."), "second");
    EXPECT_EQ(better("Same.", "Also."), "tie");
}

TEST(OracleAnswers, SeamRewritesBothParagraphsAsWritten) {
    Oracle oracle;
    teach_stages(oracle);
    const std::string_view first = "The bridge failed at four tons. [A7]";
    const std::string_view second = "Ada thinks it was young pine. [A8]";
    EXPECT_EQ(
        unwrap(oracle.chat(holding_both("seam", "first", first, "second", second), {}, {})).first().content,
        std::format("{}\n\n{}", first, second))
        << "a rewrite that passes every check, and changes nothing";
    EXPECT_ERRC(oracle.chat(holding("seam", "first", first), {}, {}), Errc::invalid_argument)
        << "no second paragraph";
    EXPECT_ERRC(oracle.chat(holding("seam", "second", second), {}, {}), Errc::invalid_argument)
        << "no first paragraph";
}

TEST(RequestLog, KeepsEveryRequestItPassesOn) {
    Oracle oracle;
    oracle.teach("label",
                 [](const llm::ChatRequest&) -> Result<std::string> { return R"({"label": "claim"})"; });
    const RequestLog log{oracle};
    EXPECT_EQ(log.name(), "oracle");
    EXPECT_TRUE(log.caps().thinking);
    EXPECT_EQ(unwrap(log.chat(asked("label", "m1"), {}, {})).first().content, R"({"label": "claim"})");
    EXPECT_ERRC(log.chat(asked("outline", "m2"), {}, {}), Errc::unsupported);
    const std::vector<llm::ChatRequest> requests = log.requests();
    ASSERT_EQ(requests.size(), 2U) << "answered or not";
    EXPECT_EQ(requests[0].tag.mission_id, "m1");
    EXPECT_EQ(requests[1].tag.mission_id, "m2");
}

TEST(Faults, EachMakesItsMistake) {
    const auto made = [](const Fault& fault, std::string content) {
        llm::Choice choice = answer(std::move(content));
        EXPECT_TRUE(fault.change(choice)) << fault.name;
        return choice;
    };

    EXPECT_EQ(made(refusal({}), "anything").content, "I'm sorry, but I can't help with that.");

    const llm::Choice cut = made(truncation({}), "Water boils.");
    EXPECT_EQ(cut.content, "Water ");
    EXPECT_EQ(cut.finish, llm::FinishReason::length);
    EXPECT_EQ(made(truncation({}), "aéé").content, "a") << "never inside a UTF-8 sequence";

    EXPECT_EQ(made(broken_json({}), R"({"atoms": [1]})").content, R"({"atoms": [1])");

    const llm::Choice thought = made(out_while_thinking({}), "an answer");
    EXPECT_TRUE(thought.content.empty());
    EXPECT_FALSE(thought.reasoning.empty());
    EXPECT_EQ(thought.finish, llm::FinishReason::length);

    EXPECT_EQ(made(wrong_number({}), "Water boils at 100 °C, 212 °F.").content,
              "Water boils at 101 °C, 212 °F.");
    EXPECT_EQ(made(wrong_number({}), "99999999999999999999 atoms").content, "999999999999999999991 atoms")
        << "too long to add to";
    EXPECT_EQ(made(bad_marker({}), "Water boils [A3], and freezes [A4].").content,
              "Water boils [A999], and freezes [A4].");

    EXPECT_EQ(made(dropped_reference({}), R"({"sentences": ["S1", "S2"]})").content,
              R"({"sentences": ["S1"]})");
    EXPECT_EQ(made(dropped_reference({}), R"({"sentences": ["S1"], "a": 1})").content,
              R"({"sentences": [], "a": 1})");

    const Fault softened = edit("softened", {}, [](const std::string& text) {
        return std::regex_replace(text, std::regex{"boils"}, "may boil");
    });
    EXPECT_EQ(made(softened, "Water boils.").content, "Water may boil.");
}

TEST(Faults, SayWhenThereWasNothingToChange) {
    llm::Choice plain = answer("no digits, no markers");
    EXPECT_FALSE(wrong_number({}).change(plain));
    EXPECT_FALSE(bad_marker({}).change(plain));
    EXPECT_FALSE(dropped_reference({}).change(plain));
    EXPECT_FALSE(broken_json({}).change(plain));
    EXPECT_FALSE(edit("noop", {}, [](std::string text) { return text; }).change(plain));
    EXPECT_EQ(plain.content, "no digits, no markers");
}

// --- Which calls a fault hits ---------------------------------------------------------

TEST(Faults, HitTheCallsTheyAreAimedAt) {
    Oracle oracle;
    oracle.teach("label",
                 [](const llm::ChatRequest&) -> Result<std::string> { return R"({"label": "claim"})"; });
    oracle.teach("judge", [](const llm::ChatRequest&) -> Result<std::string> { return R"({"verdict": 1})"; });
    const FaultyProvider faulty{
        oracle,
        {broken_json(When{.kind = "label", .calls = 1}), wrong_number(When{.mission = "j2", .calls = 0})}};

    // The first label call of each mission is broken; the second is not.
    EXPECT_EQ(unwrap(faulty.chat(asked("label", "m1"), {}, {})).choices[0].content, R"({"label": "claim")");
    EXPECT_EQ(unwrap(faulty.chat(asked("label", "m1"), {}, {})).choices[0].content, R"({"label": "claim"})");
    EXPECT_EQ(unwrap(faulty.chat(asked("label", "m2"), {}, {})).choices[0].content, R"({"label": "claim")");
    // Every call of mission j2, and no other.
    EXPECT_EQ(unwrap(faulty.chat(asked("judge", "j1"), {}, {})).choices[0].content, R"({"verdict": 1})");
    EXPECT_EQ(unwrap(faulty.chat(asked("judge", "j2"), {}, {})).choices[0].content, R"({"verdict": 2})");
    EXPECT_EQ(unwrap(faulty.chat(asked("judge", "j2"), {}, {})).choices[0].content, R"({"verdict": 2})");

    const std::vector<Injection> made = faulty.injected();
    ASSERT_EQ(made.size(), 4U);
    EXPECT_EQ(made[0].fault, "broken_json");
    EXPECT_EQ(made[0].mission, "m1");
    EXPECT_TRUE(made[0].changed);
    EXPECT_EQ(made[3].fault, "wrong_number");

    const FaultyProvider by_model{oracle,
                                  {refusal(When{.model = "qwen"}), truncation(When{.thinking = true})}};
    EXPECT_EQ(unwrap(by_model.chat(asked("label"), {}, {})).choices[0].content, R"({"label": "claim"})");
    EXPECT_THAT(unwrap(by_model.chat(asked("label", "m1", "qwen"), {}, {})).choices[0].content,
                HasSubstr("sorry"));
    llm::ChatRequest thinking = asked("label", "m3");
    thinking.thinking = true;
    EXPECT_EQ(unwrap(by_model.chat(thinking, {}, {})).choices[0].finish, llm::FinishReason::length);

    // Where two faults match a call, the first one listed is made.
    const FaultyProvider both{oracle, {refusal(When{.kind = "label"}), truncation(When{.kind = "label"})}};
    EXPECT_THAT(unwrap(both.chat(asked("label", "m9"), {}, {})).choices[0].content, HasSubstr("sorry"));
    ASSERT_EQ(both.injected().size(), 1U);
    EXPECT_EQ(both.injected()[0].fault, "refusal");

    const FaultyProvider down{oracle, {server_error(When{}, Errc::unavailable)}};
    const Result<llm::ChatResponse> failed = down.chat(asked("label"), {}, {});
    EXPECT_ERRC(failed, Errc::unavailable);
    EXPECT_THAT(failed.error().message(), HasSubstr("server_error:unavailable"));
    ASSERT_EQ(down.injected().size(), 1U) << "an error is a fault made, too";
    EXPECT_EQ(down.injected()[0].fault, "server_error:unavailable");
}

// --- The rule: every fault ends repaired, or as a finding ---------------------------------

llm::SamplingPreset direct() {
    return llm::SamplingPreset{.temperature = 0.7, .top_p = 0.8, .top_k = 20};
}

llm::SamplingPreset thinking() {
    return llm::SamplingPreset{
        .temperature = 1.0,
        .top_p = 0.95,
        .top_k = 20,
        .thinking = true,
        .thinking_budget = llm::ThinkingBudget{.tokens = 500, .closing = "Answer now."}};
}

std::vector<agents::Engine> engines() {
    return {agents::Engine{.model = "qwen",
                           .roles = {"commander"},
                           .family = "qwen",
                           .slots = 2,
                           .context = 16384,
                           .ctx_per_slot = 16384,
                           .kv_unified = true,
                           .direct = direct(),
                           .thinking = thinking()},
            agents::Engine{.model = "gemma",
                           .roles = {"infantry"},
                           .family = "gemma",
                           .slots = 2,
                           .context = 16384,
                           .ctx_per_slot = 8192,
                           .direct = direct(),
                           .thinking = thinking()}};
}

// A mission whose right answer is {"boils_at": 100, "sentences": ["S1", "S2"]}:
// the number as the packet says it, and both sentences accounted for.
agents::Mission accounting(std::string id, std::string role = "infantry") {
    static const auto schema = std::make_shared<const json::Schema>(unwrap(json::Schema::compile_text(R"({
        "type": "object",
        "additionalProperties": false,
        "required": ["sentences", "boils_at"],
        "properties": {
            "sentences": {"type": "array", "items": {"type": "string"}},
            "boils_at": {"type": "integer"}
        }
    })")));
    agents::Mission mission;
    mission.id = std::move(id);
    mission.kind = "account";
    mission.role = std::move(role);
    mission.request.messages = {llm::Message::system("Account for every sentence."),
                                llm::Message::user("S1. Water boils at 100 °C. S2. At sea level.")};
    mission.request.seed = 7;
    mission.answer_tokens = 200;
    mission.schema = schema;
    mission.check = [](const json::Json& value) {
        std::vector<std::string> problems;
        if (value["sentences"] != json::Json{"S1", "S2"}) {
            problems.emplace_back("every sentence must be accounted for, once: S1 and S2");
        }
        if (value["boils_at"] != 100) {
            problems.emplace_back("boils_at must be the packet's number, 100");
        }
        return problems;
    };
    return mission;
}

struct Rig {
    explicit Rig(std::vector<Fault> faults) : faulty(oracle, std::move(faults)) {
        oracle.teach("account", [](const llm::ChatRequest&) -> Result<std::string> {
            return R"({"boils_at": 100, "sentences": ["S1", "S2"]})";
        });
    }

    Oracle oracle;
    FaultyProvider faulty;
    FakeClock clock;
    agents::Army army =
        unwrap(agents::Army::create(agents::Army::Options{.engines = engines()}, faulty, clock));
};

TEST(Faults, EachEndsRepairedOrAsAFinding) {
    struct Case {
        Fault fault;
        agents::Verdict verdict;
        agents::Step step;
    };
    const std::vector<Case> repaired{
        {.fault = truncation({}), .verdict = agents::Verdict::accepted, .step = agents::Step::as_ordered},
        {.fault = broken_json({}), .verdict = agents::Verdict::accepted, .step = agents::Step::as_ordered},
        {.fault = wrong_number({}), .verdict = agents::Verdict::accepted, .step = agents::Step::as_ordered},
        {.fault = dropped_reference({}),
         .verdict = agents::Verdict::accepted,
         .step = agents::Step::as_ordered},
        {.fault = refusal({}), .verdict = agents::Verdict::accepted, .step = agents::Step::commander},
        // Every call wrong, everywhere: the ladder's top, and a finding.
        {.fault = wrong_number(When{.calls = 0}),
         .verdict = agents::Verdict::invalid,
         .step = agents::Step::commander},
        {.fault = refusal(When{.calls = 0}),
         .verdict = agents::Verdict::refused,
         .step = agents::Step::commander},
    };
    for (const Case& each : repaired) {
        Rig rig{{each.fault}};
        const agents::Outcome outcome = unwrap(rig.army.execute(accounting("m1")));
        EXPECT_EQ(outcome.verdict, each.verdict) << each.fault.name;
        EXPECT_EQ(outcome.step, each.step) << each.fault.name;
        EXPECT_FALSE(rig.faulty.injected().empty()) << each.fault.name << " was made";
        if (outcome.verdict != agents::Verdict::accepted) {
            EXPECT_FALSE(outcome.problems.empty()) << "a finding says why";
        }
    }
}

TEST(Faults, ThinkingThatRunsOutEndsWithAnAnswer) {
    Rig rig{{out_while_thinking(When{.thinking = true})}};
    agents::Mission outline = accounting("m1", "commander");
    outline.request.thinking = true;
    const agents::Outcome outcome = unwrap(rig.army.execute(std::move(outline)));
    EXPECT_EQ(outcome.verdict, agents::Verdict::accepted);
    ASSERT_EQ(outcome.answers.size(), 1U);
    EXPECT_FALSE(outcome.answers.front().thinking) << "asked again without thinking";
}

TEST(Faults, AServerThatStaysDownIsAnErrorNotAFinding) {
    Rig rig{{server_error(When{.calls = 0}, Errc::unavailable)}};
    EXPECT_ERRC(rig.army.execute(accounting("m1")), Errc::unavailable) << "the job stops, to resume later";
}

}  // namespace
}  // namespace rr::oracle
