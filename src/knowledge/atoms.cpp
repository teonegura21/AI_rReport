#include <rr/knowledge/atoms.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

#include <rr/knowledge/artifacts.hpp>

#include "fields.hpp"

namespace rr::knowledge {

namespace {

// The JSON names of each enumeration, in the enum's order (and the schema's).
constexpr auto kKinds = std::to_array<std::string_view>({"claim", "fact", "number", "example", "anecdote",
                                                         "aside", "joke", "hedge", "opinion", "question",
                                                         "instruction", "definition", "reference", "visual"});
constexpr auto kModalities = std::to_array<std::string_view>(
    {"asserted", "hedged", "hypothetical", "reported", "sarcastic", "question"});
constexpr auto kRegisters = std::to_array<std::string_view>({"main", "aside", "joke", "anecdote", "example"});
constexpr auto kIntensities = std::to_array<std::string_view>({"low", "medium", "high"});
constexpr auto kFillerLabels =
    std::to_array<std::string_view>({"disfluency", "greeting", "repetition", "sponsor", "decorative"});

static_assert(kKinds.size() == static_cast<std::size_t>(AtomKind::visual) + 1);
static_assert(kModalities.size() == static_cast<std::size_t>(Modality::question) + 1);
static_assert(kRegisters.size() == static_cast<std::size_t>(Register::example) + 1);
static_assert(kIntensities.size() == static_cast<std::size_t>(Intensity::high) + 1);
static_assert(kFillerLabels.size() == static_cast<std::size_t>(FillerLabel::decorative) + 1);

json::Json strings(const std::vector<std::string>& values) {
    json::Json array = json::Json::array();
    for (const std::string& value : values) {
        array.push_back(value);
    }
    return array;
}

std::vector<std::string> strings_of(const json::Json& array) {
    std::vector<std::string> values;
    values.reserve(array.size());
    for (const json::Json& value : array) {
        values.push_back(value.get<std::string>());
    }
    return values;
}

// An atom's sentences: S# IDs, strictly in order.
Result<std::vector<SentenceId>> sentences_of(const json::Json& array, std::string_view atom) {
    std::vector<SentenceId> sentences;
    for (const json::Json& value : array) {
        const std::string written = value.get<std::string>();
        const std::optional<SentenceId> id = SentenceId::parse(written);
        if (!id.has_value()) {
            return fail(Errc::invalid_argument,
                        std::format("{} cites {}, which is no sentence ID", atom, written));
        }
        if (!sentences.empty() && *id <= sentences.back()) {
            return fail(Errc::invalid_argument,
                        std::format("{} lists {} after {}: an atom's sentences are in order, once each", atom,
                                    written, sentences.back()));
        }
        sentences.push_back(*id);
    }
    return sentences;
}

Result<Atom> atom_of(const json::Json& item, AtomId expected) {
    const std::string written = item.at("id").get<std::string>();
    if (AtomId::parse(written) != expected) {
        return fail(
            Errc::invalid_argument,
            std::format("atom {} is out of place: atoms run A1, A2, \u2026 in order, and {} comes next",
                        written, expected));
    }
    RR_TRY_ASSIGN(std::vector<SentenceId> sentences, sentences_of(item.at("sentences"), written));
    RR_TRY_ASSIGN(const AtomKind kind, fields::named<AtomKind>(item.at("kind").get<std::string>(), kKinds));
    RR_TRY_ASSIGN(const Modality modality,
                  fields::named<Modality>(item.at("modality").get<std::string>(), kModalities));
    RR_TRY_ASSIGN(const Register register_kind,
                  fields::named<Register>(item.at("register").get<std::string>(), kRegisters));
    RR_TRY_ASSIGN(const Intensity intensity,
                  fields::named<Intensity>(item.at("intensity").get<std::string>(), kIntensities));
    const bool must_quote = item.at("must_quote").get<bool>();
    std::optional<std::string> quote = fields::optional_string(item.at("quote"));
    if (must_quote != quote.has_value()) {
        return fail(Errc::invalid_argument,
                    std::format("{}: a quote comes with must_quote, and must_quote with a quote", written));
    }
    return Atom{.id = expected,
                .kind = kind,
                .text = item.at("text").get<std::string>(),
                .sentences = std::move(sentences),
                .speaker = fields::optional_string(item.at("speaker")),
                .attributed_to = fields::optional_string(item.at("attributed_to")),
                .modality = modality,
                .register_kind = register_kind,
                .intensity = intensity,
                .must_quote = must_quote,
                .quote = std::move(quote),
                .names = strings_of(item.at("names")),
                .terms = strings_of(item.at("terms"))};
}

}  // namespace

std::string_view to_string(AtomKind kind) noexcept {
    return fields::name_of(kind, kKinds);
}

std::string_view to_string(Modality modality) noexcept {
    return fields::name_of(modality, kModalities);
}

std::string_view to_string(Register register_kind) noexcept {
    return fields::name_of(register_kind, kRegisters);
}

std::string_view to_string(Intensity intensity) noexcept {
    return fields::name_of(intensity, kIntensities);
}

std::string_view to_string(FillerLabel label) noexcept {
    return fields::name_of(label, kFillerLabels);
}

const Atom* AtomLedger::find(AtomId id) const noexcept {
    // A1…An sit at their number's place in a ledger that was read; one built
    // otherwise is searched.
    const std::size_t index = id.number() - 1;
    if (index < atoms.size() && atoms[index].id == id) {
        return &atoms[index];
    }
    const auto found = std::ranges::find(atoms, id, &Atom::id);
    return found == atoms.end() ? nullptr : &*found;
}

json::Json to_json(const AtomLedger& ledger) {
    json::Json atoms = json::Json::array();
    for (const Atom& atom : ledger.atoms) {
        json::Json sentences = json::Json::array();
        for (const SentenceId& sentence : atom.sentences) {
            sentences.push_back(sentence.str());
        }
        atoms.push_back(json::Json{{"id", atom.id.str()},
                                   {"kind", to_string(atom.kind)},
                                   {"text", atom.text},
                                   {"sentences", std::move(sentences)},
                                   {"speaker", fields::json_or_null(atom.speaker)},
                                   {"attributed_to", fields::json_or_null(atom.attributed_to)},
                                   {"modality", to_string(atom.modality)},
                                   {"register", to_string(atom.register_kind)},
                                   {"intensity", to_string(atom.intensity)},
                                   {"must_quote", atom.must_quote},
                                   {"quote", fields::json_or_null(atom.quote)},
                                   {"names", strings(atom.names)},
                                   {"terms", strings(atom.terms)}});
    }
    json::Json fillers = json::Json::array();
    for (const Filler& filler : ledger.fillers) {
        fillers.push_back(
            json::Json{{"sentence", filler.sentence.str()}, {"label", to_string(filler.label)}});
    }
    return json::Json{{"atoms", std::move(atoms)}, {"fillers", std::move(fillers)}};
}

Result<AtomLedger> atom_ledger_from_json(const json::Json& document) {
    RR_TRY(artifact_schemas().validate("artifacts/atoms", document));
    AtomLedger ledger;
    for (const json::Json& item : document.at("atoms")) {
        RR_TRY_ASSIGN(Atom atom, atom_of(item, AtomId{static_cast<std::uint32_t>(ledger.atoms.size() + 1)}));
        ledger.atoms.push_back(std::move(atom));
    }
    for (const json::Json& item : document.at("fillers")) {
        const std::string written = item.at("sentence").get<std::string>();
        const std::optional<SentenceId> sentence = SentenceId::parse(written);
        if (!sentence.has_value()) {
            return fail(Errc::invalid_argument,
                        std::format("a filler names {}, which is no sentence ID", written));
        }
        if (!ledger.fillers.empty() && *sentence <= ledger.fillers.back().sentence) {
            return fail(
                Errc::invalid_argument,
                std::format("the filler of {} comes after that of {}: fillers are in the order of their "
                            "sentences, one each",
                            written, ledger.fillers.back().sentence));
        }
        RR_TRY_ASSIGN(const FillerLabel label,
                      fields::named<FillerLabel>(item.at("label").get<std::string>(), kFillerLabels));
        ledger.fillers.push_back(Filler{.sentence = *sentence, .label = label});
    }
    return ledger;
}

}  // namespace rr::knowledge
