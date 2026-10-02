// What models made of a window, voted on and reduced (rr/pipeline/readings.hpp).
#include <rr/pipeline/readings.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <utility>

#include <rr/core/assert.hpp>
#include <rr/json/spans.hpp>
#include <rr/knowledge/atoms.hpp>
#include <rr/text/tokens.hpp>

namespace rr::pipeline {

namespace {

using knowledge::Atom;
using knowledge::SentenceId;

// The schemas allow S1, S2, … only.
SentenceId sentence_of(const json::Json& written) {
    const std::optional<SentenceId> id = SentenceId::parse(written.get<std::string>());
    RR_ASSERT(id.has_value(), "the answer's schema allows S1, S2, \u2026 only");
    return *id;
}

bool cites(const Atom& atom, SentenceId id) {
    return std::ranges::contains(atom.sentences, id);
}

// Where `id` is among `ids`; none when it is not there.
std::optional<std::size_t> index_in(std::span<const SentenceId> ids, SentenceId id) {
    const auto found = std::ranges::find(ids, id);
    return found == ids.end() ? std::nullopt : std::optional{static_cast<std::size_t>(found - ids.begin())};
}

// Atoms in the order of their first sentences, then as read; renumbered.
void renumber(std::vector<Atom>& atoms) {
    std::ranges::stable_sort(atoms, {}, [](const Atom& atom) { return atom.sentences.front(); });
    for (std::size_t index = 0; index < atoms.size(); ++index) {
        atoms[index].id = knowledge::AtomId{static_cast<std::uint32_t>(index + 1)};
    }
}

// Each atom of `reading`, as the stretch of `ids` it spans; none for an atom
// that cites none of them.
std::vector<Stretch> extents_of(const Reading& reading, std::span<const SentenceId> ids) {
    std::vector<Stretch> extents;
    extents.reserve(reading.atoms.size());
    for (const Atom& atom : reading.atoms) {
        const std::optional<std::size_t> first = index_in(ids, atom.sentences.front());
        const std::optional<std::size_t> last = index_in(ids, atom.sentences.back());
        if (first.has_value() && last.has_value()) {
            extents.push_back(Stretch{.begin = *first, .end = *last + 1});
        }
    }
    return extents;
}

// Whether `extent` overlaps `stretch` and reaches past one of its edges: an
// atom that the stretch would cut in two.
bool crosses(const Stretch& extent, const Stretch& stretch) {
    return extent.begin < stretch.end && extent.end > stretch.begin &&
           (extent.begin < stretch.begin || extent.end > stretch.end);
}

// `stretches` in order, those that overlap or touch made one.
std::vector<Stretch> joined(std::vector<Stretch> stretches) {
    std::ranges::sort(stretches, {}, &Stretch::begin);
    std::vector<Stretch> kept;
    for (const Stretch& stretch : stretches) {
        if (!kept.empty() && stretch.begin <= kept.back().end) {
            kept.back().end = std::max(kept.back().end, stretch.end);
        } else {
            kept.push_back(stretch);
        }
    }
    return kept;
}

// `stretches`, each grown over the extents that cross its edges and joined
// with those it then meets, until no extent crosses an edge.
std::vector<Stretch> grown(std::vector<Stretch> stretches, std::span<const Stretch> extents) {
    for (bool growing = true; growing;) {
        growing = false;
        for (Stretch& stretch : stretches) {
            for (const Stretch& extent : extents) {
                if (crosses(extent, stretch)) {
                    stretch = Stretch{.begin = std::min(stretch.begin, extent.begin),
                                      .end = std::max(stretch.end, extent.end)};
                    growing = true;
                }
            }
        }
        stretches = joined(std::move(stretches));
    }
    return stretches;
}

// Bytes of an answer's text: [begin, end).
struct Bytes {
    std::size_t begin = 0;
    std::size_t end = 0;
};

using SpansByPointer = std::map<std::string, json::Span, std::less<>>;

// The bytes of the value at `pointer`: an accepted answer has every value
// its schema requires.
Bytes value_at(const SpansByPointer& spans, const std::string& pointer) {
    const auto found = spans.find(pointer);
    RR_ASSERT(found != spans.end(), "an accepted answer has every field its schema requires");
    return Bytes{.begin = found->second.begin, .end = found->second.end};
}

// The bytes that choose an atom's labels: its kind, its modality, and the
// first byte of its must_quote, which is null or a quote.
std::vector<Bytes> labels_of(const SpansByPointer& spans, const std::string& atom) {
    const Bytes quote = value_at(spans, atom + "/must_quote");
    return {value_at(spans, atom + "/kind"), value_at(spans, atom + "/modality"),
            Bytes{.begin = quote.begin, .end = quote.begin + 1}};
}

// The bytes that choose how many atoms the list at `list` holds: from its
// bracket to its first atom's brace (any at all?), and after each atom, to
// the next one's brace or to the closing bracket (one more?).
void add_count(std::vector<Bytes>& decisions, const SpansByPointer& spans, const std::string& list,
               std::size_t atoms) {
    const Bytes whole = value_at(spans, list);
    std::size_t from = whole.begin;
    for (std::size_t index = 0; index < atoms; ++index) {
        const Bytes atom = value_at(spans, std::format("{}/{}", list, index));
        decisions.push_back(Bytes{.begin = from, .end = atom.begin + 1});
        from = atom.end;
    }
    decisions.push_back(Bytes{.begin = from, .end = whole.end});
}

// The product of the probabilities of the tokens that overlap any of
// `decisions`, each token once; none when a byte of them is covered by no
// token. `tokens` are in order and apart.
std::optional<double> probability_of(std::span<const llm::TokenProbability> tokens,
                                     std::span<const Bytes> decisions) {
    std::set<std::size_t> counted;  // the tokens, by index
    for (const Bytes& bytes : decisions) {
        std::size_t covered = bytes.begin;
        // The first token that ends after the decision begins.
        auto token = std::ranges::upper_bound(tokens, bytes.begin, {}, &llm::TokenProbability::end);
        for (; token != tokens.end() && token->begin < bytes.end; ++token) {
            if (token->begin > covered) {
                return std::nullopt;  // a byte no token's probability came with
            }
            counted.insert(static_cast<std::size_t>(token - tokens.begin()));
            covered = token->end;
        }
        if (covered < bytes.end) {
            return std::nullopt;
        }
    }
    double probability = 1.0;
    for (const std::size_t index : counted) {
        probability *= tokens[index].probability;
    }
    return probability;
}

}  // namespace

json::Json artifact_of(const json::Json& answer) {
    json::Json atoms = json::Json::array();
    json::Json fillers = json::Json::array();
    for (const json::Json& row : answer.at("sentences")) {
        const SentenceId sentence = sentence_of(row.at("sentence"));
        if (!row.at("filler").is_null()) {
            fillers.push_back(json::Json{{"sentence", sentence.str()}, {"label", row.at("filler")}});
        }
        for (const json::Json& atom : row.at("atoms")) {
            std::vector<SentenceId> cited{sentence};
            for (const json::Json& also : atom.at("also")) {
                cited.push_back(sentence_of(also));
            }
            std::ranges::sort(cited);
            const auto repeats = std::ranges::unique(cited);
            cited.erase(repeats.begin(), repeats.end());
            json::Json sentences = json::Json::array();
            for (const SentenceId id : cited) {
                sentences.push_back(id.str());
            }
            const json::Json& quote = atom.at("must_quote");
            atoms.push_back(json::Json{{"id", std::format("A{}", atoms.size() + 1)},
                                       {"kind", atom.at("kind")},
                                       {"text", atom.at("text")},
                                       {"sentences", std::move(sentences)},
                                       {"speaker", atom.at("speaker")},
                                       {"attributed_to", atom.at("attributed_to")},
                                       {"modality", atom.at("modality")},
                                       {"register", atom.at("register")},
                                       {"intensity", atom.at("intensity")},
                                       {"must_quote", !quote.is_null()},
                                       {"quote", quote},
                                       {"names", atom.at("names")},
                                       {"terms", atom.at("terms")}});
        }
    }
    return json::Json{{"atoms", std::move(atoms)}, {"fillers", std::move(fillers)}};
}

Result<Reading> reading_of(const json::Json& answer) {
    RR_TRY_ASSIGN(knowledge::AtomLedger ledger, knowledge::atom_ledger_from_json(artifact_of(answer)));
    return Reading{.atoms = std::move(ledger.atoms), .fillers = std::move(ledger.fillers)};
}

std::string verdict_on(const Reading& reading, SentenceId id) {
    const auto filler = std::ranges::find(reading.fillers, id, &knowledge::Filler::sentence);
    if (filler != reading.fillers.end()) {
        return std::format("filler {}", knowledge::to_string(filler->label));
    }
    std::vector<std::string> parts;
    for (const Atom& atom : reading.atoms) {
        if (cites(atom, id)) {
            parts.push_back(std::format("{}/{}{}", knowledge::to_string(atom.kind),
                                        knowledge::to_string(atom.modality),
                                        atom.must_quote ? "/quote" : ""));
        }
    }
    if (parts.empty()) {
        return {};
    }
    std::ranges::sort(parts);
    std::string verdict = "atoms";
    for (const std::string& part : parts) {
        verdict += (verdict == "atoms" ? " " : ", ") + part;
    }
    return verdict;
}

std::map<SentenceId, double> confidence_of(std::string_view answer,
                                           std::span<const llm::TokenProbability> tokens) {
    std::map<SentenceId, double> confidence;
    if (tokens.empty()) {
        return confidence;  // none asked for, or none sent: nothing is known
    }
    const Result<json::Json> document = json::parse(answer);
    const Result<std::vector<json::Span>> found = json::spans(answer);
    RR_ASSERT(document.has_value() && found.has_value(), "an accepted answer is JSON");
    SpansByPointer spans;
    for (const json::Span& span : *found) {
        spans.emplace(span.pointer, span);
    }
    // The bytes that choose each sentence's verdict (the header comment).
    std::map<SentenceId, std::vector<Bytes>> decisions;
    const json::Json& rows = document->at("sentences");
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const std::string row = std::format("/sentences/{}", index);
        std::vector<Bytes>& own = decisions[sentence_of(rows[index].at("sentence"))];
        own.push_back(value_at(spans, row + "/filler"));
        const json::Json& atoms = rows[index].at("atoms");
        add_count(own, spans, row + "/atoms", atoms.size());
        for (std::size_t at = 0; at < atoms.size(); ++at) {
            const std::string atom = std::format("{}/atoms/{}", row, at);
            const std::vector<Bytes> labels = labels_of(spans, atom);
            own.insert(own.end(), labels.begin(), labels.end());
            const json::Json& also = atoms[at].at("also");
            for (std::size_t named = 0; named < also.size(); ++named) {
                std::vector<Bytes>& theirs = decisions[sentence_of(also[named])];
                theirs.insert(theirs.end(), labels.begin(), labels.end());
                theirs.push_back(value_at(spans, std::format("{}/also/{}", atom, named)));
            }
        }
    }
    for (const auto& [id, bytes] : decisions) {
        if (const std::optional<double> probability = probability_of(tokens, bytes);
            probability.has_value()) {
            confidence.emplace(id, *probability);
        }
    }
    return confidence;
}

std::vector<reason::Ballot> ballots_of(std::span<const Reading> readings, std::span<const SentenceId> ids) {
    std::vector<reason::Ballot> ballots;
    ballots.reserve(readings.size() * ids.size());
    for (std::size_t sample = 0; sample < readings.size(); ++sample) {
        for (const SentenceId id : ids) {
            std::string text;
            for (const Atom& atom : readings[sample].atoms) {
                if (cites(atom, id)) {
                    text += text.empty() ? atom.text : " " + atom.text;
                }
            }
            const auto sure = readings[sample].confidence.find(id);
            ballots.push_back(reason::Ballot{.item = id.str(),
                                             .value = verdict_on(readings[sample], id),
                                             .text = std::move(text),
                                             .sample = static_cast<int>(sample),
                                             .confidence = sure != readings[sample].confidence.end()
                                                               ? std::optional{sure->second}
                                                               : std::nullopt});
        }
    }
    return ballots;
}

std::size_t base_of(std::span<const Reading> readings, std::span<const SentenceId> ids,
                    std::span<const reason::Consensus> votes) {
    RR_ASSERT(votes.size() == ids.size(), "a vote for each sentence");
    std::size_t base = 0;
    std::size_t most = 0;
    for (std::size_t sample = 0; sample < readings.size(); ++sample) {
        std::size_t won = 0;
        for (std::size_t index = 0; index < ids.size(); ++index) {
            if (votes[index].winner == verdict_on(readings[sample], ids[index])) {
                ++won;
            }
        }
        if (won > most) {
            most = won;
            base = sample;
        }
    }
    return base;
}

std::vector<Stretch> unsettled(const Reading& base, std::span<const SentenceId> ids,
                               std::span<const reason::Consensus> votes) {
    RR_ASSERT(votes.size() == ids.size(), "a vote for each sentence");
    std::vector<Stretch> stretches;  // each sentence alone: grown() joins those that meet
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (votes[index].winner != verdict_on(base, ids[index])) {
            stretches.push_back(Stretch{.begin = index, .end = index + 1});
        }
    }
    return grown(std::move(stretches), extents_of(base, ids));
}

Reading replaced(Reading base, std::span<const SentenceId> ids, Stretch stretch, const Reading& again) {
    const auto inside = [&](SentenceId id) {
        const std::optional<std::size_t> index = index_in(ids, id);
        return index.has_value() && *index >= stretch.begin && *index < stretch.end;
    };
    std::erase_if(base.atoms, [&](const Atom& atom) { return inside(atom.sentences.front()); });
    std::erase_if(base.fillers, [&](const knowledge::Filler& filler) { return inside(filler.sentence); });
    base.atoms.insert(base.atoms.end(), again.atoms.begin(), again.atoms.end());
    base.fillers.insert(base.fillers.end(), again.fillers.begin(), again.fillers.end());
    std::ranges::stable_sort(base.fillers, {}, &knowledge::Filler::sentence);
    renumber(base.atoms);
    return base;
}

double coverage(const knowledge::Sentence& sentence, std::span<const Atom> atoms) {
    const std::vector<text::Term> words = text::terms(sentence.text);
    if (words.empty()) {
        return 1.0;
    }
    std::set<std::string> held;
    const auto hold = [&](std::string_view text) {
        for (text::Term& term : text::terms(text)) {
            held.insert(std::move(term.folded));
        }
    };
    for (const Atom& atom : atoms) {
        hold(atom.text);
        for (const std::string& name : atom.names) {
            hold(name);
        }
        for (const std::string& term : atom.terms) {
            hold(term);
        }
        if (atom.quote.has_value()) {
            hold(*atom.quote);
        }
    }
    const auto found =
        std::ranges::count_if(words, [&](const text::Term& word) { return held.contains(word.folded); });
    return static_cast<double>(found) / static_cast<double>(words.size());
}

knowledge::AtomLedger ledger_of(std::span<const Reading> windows) {
    knowledge::AtomLedger ledger;
    for (const Reading& reading : windows) {
        ledger.atoms.insert(ledger.atoms.end(), reading.atoms.begin(), reading.atoms.end());
        ledger.fillers.insert(ledger.fillers.end(), reading.fillers.begin(), reading.fillers.end());
    }
    renumber(ledger.atoms);  // a recalled atom was added at its window's end
    return ledger;
}

}  // namespace rr::pipeline
