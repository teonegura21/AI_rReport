// Whether an atom may be declared a duplicate of another (rr/fidelity/checks.hpp).
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/fidelity/checks.hpp>
#include <rr/text/align.hpp>
#include <rr/text/numbers.hpp>
#include <rr/text/span.hpp>
#include <rr/text/unicode.hpp>

#include "held.hpp"

namespace rr::fidelity {

namespace {

using knowledge::Atom;
using knowledge::Modality;

std::optional<std::string> folded(const std::optional<std::string>& text) {
    if (!text.has_value()) {
        return std::nullopt;
    }
    return text::fold(*text).value_or(*text);
}

// "hedged", "a question": how an atom is said, after "it is".
std::string said_as(Modality modality) {
    return modality == Modality::question ? std::string{"a question"}
                                          : std::string{knowledge::to_string(modality)};
}

std::string by(const std::optional<std::string>& speaker) {
    return speaker.value_or("a speaker the transcript does not name");
}

std::string reporting(const std::optional<std::string>& source) {
    return source.has_value() ? "reports " + *source : std::string{"reports no one"};
}

}  // namespace

std::vector<std::string> duplicate_problems(const Atom& atom, const Atom& of) {
    std::vector<std::string> problems;
    // What telling `of` puts in the article: its words, its names and terms.
    std::string told = of.text;
    for (const std::string& name : of.names) {
        told += "\n" + name;
    }
    for (const std::string& term : of.terms) {
        told += "\n" + term;
    }
    const std::vector<text::NumberMention> stated = held::numbers(of.text);
    for (const text::NumberMention& number : held::numbers(atom.text)) {
        if (!held::states(stated, number)) {
            problems.push_back(std::format("it says \"{}\", which {} does not",
                                           text::text_of(atom.text, number.span), of.id));
        }
    }
    for (const std::string& name : atom.names) {
        if (!held::names(told, name)) {
            problems.push_back(std::format("it names \"{}\", which {} does not", name, of.id));
        }
    }
    for (const std::string& term : atom.terms) {
        if (!held::uses(told, term)) {
            problems.push_back(std::format("it uses \"{}\", which {} does not", term, of.id));
        }
    }
    if (atom.quote.has_value() && !(of.quote.has_value() && text::align(*atom.quote, *of.quote).verbatim())) {
        problems.push_back(std::format("its words \"{}\" are to be quoted, and {} does not quote them",
                                       held::excerpt(*atom.quote), of.id));
    }
    if (atom.modality != of.modality) {
        problems.push_back(
            std::format("it is {}, and {} {}", said_as(atom.modality), of.id, said_as(of.modality)));
    }
    if (folded(atom.speaker) != folded(of.speaker)) {
        problems.push_back(
            std::format("it is said by {}, and {} by {}", by(atom.speaker), of.id, by(of.speaker)));
    }
    if (folded(atom.attributed_to) != folded(of.attributed_to)) {
        problems.push_back(std::format("it {}, and {} {}", reporting(atom.attributed_to), of.id,
                                       reporting(of.attributed_to)));
    }
    return problems;
}

}  // namespace rr::fidelity
