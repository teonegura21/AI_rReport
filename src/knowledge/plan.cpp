// How the article retells the atoms (rr/knowledge/plan.hpp).
#include <rr/knowledge/plan.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include <rr/core/assert.hpp>
#include <rr/knowledge/artifacts.hpp>

#include "fields.hpp"

namespace rr::knowledge {

namespace {

constexpr std::array<std::string_view, 2> kPlannedBy{"commander", "grouping"};

// The schema allows S1, S2, … only.
SentenceId sentence(const json::Json& written) {
    const std::optional<SentenceId> id = SentenceId::parse(written.get<std::string>());
    RR_ASSERT(id.has_value(), "the plan's schema allows S1, S2, \u2026 only");
    return *id;
}

// The schema allows A1, A2, … only.
AtomId atom(const json::Json& written) {
    const std::optional<AtomId> id = AtomId::parse(written.get<std::string>());
    RR_ASSERT(id.has_value(), "the plan's schema allows A1, A2, \u2026 only");
    return *id;
}

json::Json ids(const std::vector<AtomId>& atoms) {
    json::Json written = json::Json::array();
    for (const AtomId id : atoms) {
        written.push_back(id.str());
    }
    return written;
}

// The rules of the header comment that no schema says.
Result<void> check(const Plan& plan) {
    std::set<AtomId> told;
    for (std::size_t index = 0; index < plan.sections.size(); ++index) {
        const SectionPlan& section = plan.sections[index];
        if (index > 0 && !(plan.sections[index - 1].first < section.first)) {
            return fail(Errc::invalid_argument,
                        std::format("the section at {} comes after the one at {}: sections run in the order "
                                    "said, each once",
                                    section.first, plan.sections[index - 1].first));
        }
        for (const Paragraph& paragraph : section.paragraphs) {
            for (const AtomId id : paragraph.atoms) {
                if (!told.insert(id).second) {
                    return fail(Errc::invalid_argument,
                                std::format("{} is told twice: an atom is told once, in one paragraph", id));
                }
            }
        }
    }
    std::set<AtomId> declared;
    for (const Duplicate& duplicate : plan.duplicates) {
        if (duplicate.atom == duplicate.of) {
            return fail(Errc::invalid_argument,
                        std::format("{} is declared a duplicate of itself", duplicate.atom));
        }
        if (told.contains(duplicate.atom)) {
            return fail(
                Errc::invalid_argument,
                std::format("{} is told and declared a duplicate: it is one or the other", duplicate.atom));
        }
        if (!declared.insert(duplicate.atom).second) {
            return fail(Errc::invalid_argument,
                        std::format("{} is declared a duplicate twice", duplicate.atom));
        }
        if (!told.contains(duplicate.of)) {
            return fail(Errc::invalid_argument,
                        std::format("{} is declared a duplicate of {}, which no paragraph tells",
                                    duplicate.atom, duplicate.of));
        }
    }
    return ok();
}

// The outline's section that holds `sentence`, or none.
const Section* section_holding(const Outline& outline, SentenceId sentence) {
    for (const Section& section : outline.sections) {
        if (section.first <= sentence && sentence <= section.last) {
            return &section;
        }
    }
    return nullptr;
}

}  // namespace

std::string_view to_string(PlannedBy by) noexcept {
    return fields::name_of(by, kPlannedBy);
}

json::Json to_json(const Plan& plan) {
    json::Json sections = json::Json::array();
    for (const SectionPlan& section : plan.sections) {
        json::Json paragraphs = json::Json::array();
        for (const Paragraph& paragraph : section.paragraphs) {
            paragraphs.push_back(json::Json{{"purpose", fields::json_or_null(paragraph.purpose)},
                                            {"atoms", ids(paragraph.atoms)}});
        }
        sections.push_back(json::Json{{"first", section.first.str()},
                                      {"heading", section.heading},
                                      {"paragraphs", std::move(paragraphs)},
                                      {"planned_by", std::string{to_string(section.planned_by)}}});
    }
    json::Json duplicates = json::Json::array();
    for (const Duplicate& duplicate : plan.duplicates) {
        duplicates.push_back(json::Json{{"atom", duplicate.atom.str()},
                                        {"of", duplicate.of.str()},
                                        {"because", fields::json_or_null(duplicate.because)}});
    }
    return json::Json{{"sections", std::move(sections)}, {"duplicates", std::move(duplicates)}};
}

Result<Plan> plan_from_json(const json::Json& document) {
    RR_TRY(artifact_schemas().validate("artifacts/plan", document));
    Plan plan;
    for (const json::Json& item : document.at("sections")) {
        SectionPlan section{.first = sentence(item.at("first")),
                            .heading = item.at("heading").get<std::string>()};
        for (const json::Json& written : item.at("paragraphs")) {
            Paragraph paragraph{.purpose = fields::optional_string(written.at("purpose"))};
            for (const json::Json& id : written.at("atoms")) {
                paragraph.atoms.push_back(atom(id));
            }
            section.paragraphs.push_back(std::move(paragraph));
        }
        RR_TRY_ASSIGN(section.planned_by,
                      fields::named<PlannedBy>(item.at("planned_by").get<std::string>(), kPlannedBy));
        plan.sections.push_back(std::move(section));
    }
    for (const json::Json& item : document.at("duplicates")) {
        plan.duplicates.push_back(Duplicate{.atom = atom(item.at("atom")),
                                            .of = atom(item.at("of")),
                                            .because = fields::optional_string(item.at("because"))});
    }
    RR_TRY(check(plan));
    return plan;
}

std::vector<std::string> plan_problems(const Plan& plan, const AtomLedger& ledger, const Outline& outline) {
    std::vector<std::string> problems;
    std::set<SentenceId> planned;
    for (const SectionPlan& section : plan.sections) {
        planned.insert(section.first);
    }
    std::set<SentenceId> outlined;
    for (const Section& section : outline.sections) {
        outlined.insert(section.first);
        if (!planned.contains(section.first)) {
            problems.push_back(
                std::format("the outline's section at {}, \"{}\", is not planned: plan every "
                            "section of the outline",
                            section.first, section.title));
        }
    }
    std::set<AtomId> placed;  // told, or declared a duplicate
    for (const SectionPlan& section : plan.sections) {
        if (!outlined.contains(section.first)) {
            problems.push_back(
                std::format("the plan has a section at {}, which the outline has not: plan the "
                            "outline's sections only",
                            section.first));
            continue;
        }
        for (const Paragraph& paragraph : section.paragraphs) {
            for (const AtomId id : paragraph.atoms) {
                placed.insert(id);
                const Atom* const told = ledger.find(id);
                if (told == nullptr) {
                    problems.push_back(std::format(
                        "{} is told in the section at {}, and the ledger has no {}", id, section.first, id));
                    continue;
                }
                const SentenceId said = told->sentences.front();
                const Section* const holding = section_holding(outline, said);
                if (holding != nullptr && holding->first != section.first) {
                    problems.push_back(
                        std::format("{}, said at {}, is told in the section at {}: tell it in the "
                                    "section at {}, where it was said",
                                    id, said, section.first, holding->first));
                }
            }
        }
    }
    for (const Duplicate& duplicate : plan.duplicates) {
        placed.insert(duplicate.atom);
        if (ledger.find(duplicate.atom) == nullptr) {
            problems.push_back(std::format("{} is declared a duplicate, and the ledger has no {}",
                                           duplicate.atom, duplicate.atom));
        }
    }
    for (const Atom& atom : ledger.atoms) {
        if (!placed.contains(atom.id)) {
            problems.push_back(std::format(
                "{} is neither told nor declared a duplicate: every atom of the ledger reaches the article",
                atom.id));
        }
    }
    return problems;
}

}  // namespace rr::knowledge
