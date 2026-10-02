// A section's atoms grouped in the order said: the floor every plan has.
#include <rr/pipeline/grouping.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace rr::pipeline {
namespace {

using knowledge::Atom;
using knowledge::AtomId;
using knowledge::Paragraph;
using knowledge::Register;

std::vector<Atom> atoms(std::uint32_t count) {
    std::vector<Atom> made;
    made.reserve(count);
    for (std::uint32_t number = 1; number <= count; ++number) {
        made.push_back(Atom{.id = AtomId{number}, .speaker = "Ada"});
    }
    return made;
}

std::vector<const Atom*> pointers(const std::vector<Atom>& atoms) {
    std::vector<const Atom*> to;
    to.reserve(atoms.size());
    for (const Atom& atom : atoms) {
        to.push_back(&atom);
    }
    return to;
}

std::vector<std::vector<std::uint32_t>> numbers(const std::vector<Paragraph>& paragraphs) {
    std::vector<std::vector<std::uint32_t>> all;
    for (const Paragraph& paragraph : paragraphs) {
        EXPECT_EQ(paragraph.purpose, std::nullopt) << "no model said what a grouped paragraph is for";
        all.emplace_back();
        for (const AtomId id : paragraph.atoms) {
            all.back().push_back(id.number());
        }
    }
    return all;
}

TEST(Grouping, TellsSixAtomsAParagraphInTheOrderSaid) {
    const std::vector<Atom> thirteen = atoms(13);
    EXPECT_EQ(numbers(grouped(pointers(thirteen))),
              (std::vector<std::vector<std::uint32_t>>{{1, 2, 3, 4, 5, 6}, {7, 8, 9, 10, 11, 12}, {13}}));
    EXPECT_TRUE(grouped({}).empty()) << "no atoms, no paragraph";
}

TEST(Grouping, StartsAParagraphWhereTheSpeakerOrTheRegisterChanges) {
    std::vector<Atom> talk = atoms(7);
    talk[2].speaker = "Ben";  // Ben answers…
    talk[3].speaker = "Ben";  // …at some length
    talk[4].speaker.reset();  // someone the transcript does not name
    talk[6].register_kind = Register::joke;
    EXPECT_EQ(numbers(grouped(pointers(talk))),
              (std::vector<std::vector<std::uint32_t>>{{1, 2}, {3, 4}, {5}, {6}, {7}}));
}

}  // namespace
}  // namespace rr::pipeline
