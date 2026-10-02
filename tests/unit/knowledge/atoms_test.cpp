#include <rr/knowledge/atoms.hpp>

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <rr/core/error.hpp>
#include <rr/knowledge/artifacts.hpp>
#include <rr/testkit/result.hpp>

namespace rr::knowledge {
namespace {

using testkit::unwrap;

AtomLedger ledger() {
    return AtomLedger{
        .atoms =
            {
                Atom{.id = AtomId{1},
                     .kind = AtomKind::fact,
                     .text = "Water boils at 100 degrees at sea level.",
                     .sentences = {SentenceId{2}, SentenceId{3}},
                     .speaker = "Ada"},
                Atom{.id = AtomId{2},
                     .kind = AtomKind::opinion,
                     .text = "Boiling eggs is overrated.",
                     .sentences = {SentenceId{4}},
                     .speaker = "Ada",
                     .attributed_to = "her grandmother",
                     .modality = Modality::reported,
                     .register_kind = Register::aside,
                     .intensity = Intensity::high,
                     .must_quote = true,
                     .quote = "Boiling eggs is a crime",
                     .names = {"Ada Lovelace"},
                     .terms = {"soft-boiled"}},
            },
        .fillers = {Filler{.sentence = SentenceId{1}, .label = FillerLabel::greeting},
                    Filler{.sentence = SentenceId{5}, .label = FillerLabel::sponsor}},
    };
}

TEST(Atoms, RoundTripThroughTheirArtifact) {
    const AtomLedger original = ledger();
    EXPECT_EQ(unwrap(atom_ledger_from_json(to_json(original))), original);
}

TEST(Atoms, AreWrittenWithTheirSchemasNames) {
    const json::Json document = to_json(ledger());
    const json::Json& atom = document.at("atoms").at(1);
    EXPECT_EQ(atom.at("kind"), "opinion");
    EXPECT_EQ(atom.at("modality"), "reported");
    EXPECT_EQ(atom.at("register"), "aside");
    EXPECT_EQ(atom.at("intensity"), "high");
    EXPECT_EQ(document.at("fillers").at(1).at("label"), "sponsor");
    EXPECT_EQ(document.at("atoms").at(0).at("attributed_to"), nullptr);
}

TEST(Atoms, EveryValueOfEveryEnumerationIsTheSchemas) {
    // Each C++ value round-trips: the names in code and in the schema agree.
    AtomLedger all = ledger();
    for (int kind = 0; kind <= static_cast<int>(AtomKind::visual); ++kind) {
        all.atoms[0].kind = static_cast<AtomKind>(kind);
        EXPECT_EQ(unwrap(atom_ledger_from_json(to_json(all))).atoms[0].kind, all.atoms[0].kind)
            << to_string(all.atoms[0].kind);
    }
    for (int modality = 0; modality <= static_cast<int>(Modality::question); ++modality) {
        all.atoms[0].modality = static_cast<Modality>(modality);
        EXPECT_EQ(unwrap(atom_ledger_from_json(to_json(all))).atoms[0].modality, all.atoms[0].modality);
    }
    for (int register_kind = 0; register_kind <= static_cast<int>(Register::example); ++register_kind) {
        all.atoms[0].register_kind = static_cast<Register>(register_kind);
        EXPECT_EQ(unwrap(atom_ledger_from_json(to_json(all))).atoms[0].register_kind,
                  all.atoms[0].register_kind);
    }
    for (int intensity = 0; intensity <= static_cast<int>(Intensity::high); ++intensity) {
        all.atoms[0].intensity = static_cast<Intensity>(intensity);
        EXPECT_EQ(unwrap(atom_ledger_from_json(to_json(all))).atoms[0].intensity, all.atoms[0].intensity);
    }
    for (int label = 0; label <= static_cast<int>(FillerLabel::decorative); ++label) {
        all.fillers[0].label = static_cast<FillerLabel>(label);
        EXPECT_EQ(unwrap(atom_ledger_from_json(to_json(all))).fillers[0].label, all.fillers[0].label);
    }
}

TEST(Atoms, AreFoundByTheirId) {
    const AtomLedger atoms = ledger();
    ASSERT_NE(atoms.find(AtomId{2}), nullptr);
    EXPECT_EQ(atoms.find(AtomId{2})->kind, AtomKind::opinion);
    EXPECT_EQ(atoms.find(AtomId{3}), nullptr);
    const AtomLedger out_of_place{.atoms = {ledger().atoms[1]}};
    ASSERT_NE(out_of_place.find(AtomId{2}), nullptr) << "a ledger built in code is searched";
    EXPECT_EQ(out_of_place.find(AtomId{1}), nullptr);
}

void expect_refused(const json::Json& document, std::string_view because) {
    const Result<AtomLedger> read = atom_ledger_from_json(document);
    ASSERT_FALSE(read.has_value()) << because;
    EXPECT_EQ(read.error().code(), Errc::invalid_argument);
    EXPECT_NE(read.error().message().find(because), std::string::npos) << read.error().message();
}

TEST(Atoms, RunFromA1InOrder) {
    json::Json document = to_json(ledger());
    document["atoms"][1]["id"] = "A3";
    expect_refused(document, "atom A3 is out of place");
}

TEST(Atoms, CiteTheirSentencesInOrderOnceEach) {
    json::Json document = to_json(ledger());
    document["atoms"][0]["sentences"] = json::Json::array({"S3", "S2"});
    expect_refused(document, "A1 lists S2 after S3");
    document["atoms"][0]["sentences"] = json::Json::array({"S2", "S2"});
    expect_refused(document, "A1 lists S2 after S2");
    document["atoms"][0]["sentences"] = json::Json::array();
    expect_refused(document, "schema violation");
}

TEST(Atoms, QuoteExactlyWhenTheyMust) {
    json::Json document = to_json(ledger());
    document["atoms"][1]["quote"] = nullptr;
    expect_refused(document, "A2: a quote comes with must_quote");
    document = to_json(ledger());
    document["atoms"][0]["quote"] = "Water boils";
    expect_refused(document, "A1: a quote comes with must_quote");
}

TEST(Atoms, FillersAreInTheOrderOfTheirSentencesOneEach) {
    json::Json document = to_json(ledger());
    document["fillers"][1]["sentence"] = "S1";
    expect_refused(document, "the filler of S1 comes after that of S1");
}

TEST(Atoms, LabelsAreTheClosedList) {
    json::Json document = to_json(ledger());
    document["fillers"][0]["label"] = "boring";
    expect_refused(document, "schema violation");
    document = to_json(ledger());
    document["atoms"][0]["kind"] = "rumour";
    expect_refused(document, "schema violation");
}

TEST(ArtifactSchemas, AreCompiledIn) {
    EXPECT_EQ(artifact_schemas().names(),
              (std::vector<std::string>{"artifacts/article", "artifacts/atoms", "artifacts/judgement",
                                        "artifacts/outline", "artifacts/pairing", "artifacts/plan",
                                        "artifacts/silver", "artifacts/source", "artifacts/transcript"}));
}

}  // namespace
}  // namespace rr::knowledge
