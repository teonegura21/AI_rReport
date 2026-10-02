// A section's atoms grouped in the order said (rr/pipeline/grouping.hpp).
#include <rr/pipeline/grouping.hpp>

namespace rr::pipeline {

std::vector<knowledge::Paragraph> grouped(std::span<const knowledge::Atom* const> atoms) {
    std::vector<knowledge::Paragraph> paragraphs;
    const knowledge::Atom* previous = nullptr;
    for (const knowledge::Atom* const atom : atoms) {
        const bool turns = previous != nullptr && (atom->speaker != previous->speaker ||
                                                   atom->register_kind != previous->register_kind);
        if (paragraphs.empty() || turns || paragraphs.back().atoms.size() == kGroupedAtoms) {
            paragraphs.emplace_back();
        }
        paragraphs.back().atoms.push_back(atom->id);
        previous = atom;
    }
    return paragraphs;
}

}  // namespace rr::pipeline
