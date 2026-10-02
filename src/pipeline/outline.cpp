// How a video's sections are made and joined (rr/pipeline/outline.hpp).
#include <rr/pipeline/outline.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <functional>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

#include <rr/core/assert.hpp>
#include <rr/reason/boundaries.hpp>
#include <rr/text/unicode.hpp>

namespace rr::pipeline {

namespace {

using knowledge::Transcript;

// The index of the sentence after the section that `cuts[index]` starts.
std::size_t section_end(std::span<const Cut> cuts, std::size_t index, const Transcript& transcript) {
    return index + 1 < cuts.size() ? cuts[index + 1].first : transcript.sentences.size();
}

Millis stretch_length(const Transcript& transcript, std::size_t first, std::size_t end) {
    return transcript.sentences[end - 1].span.end() - transcript.sentences[first].span.start();
}

// Where to cut the sentences [first, end), two or more: before the sentence
// after the longest pause, among the cuts that leave each part a quarter of
// the stretch at least; on a tie, and when no cut does, the cut nearest the
// middle; on a tie again, the earlier.
std::size_t pause_cut(const Transcript& transcript, std::size_t first, std::size_t end) {
    const Millis start = transcript.sentences[first].span.start();
    const Millis length = stretch_length(transcript, first, end);
    // Better cuts have greater keys: balanced, then a longer pause, then nearer the middle.
    const auto key = [&](std::size_t cut) {
        const Millis at = transcript.sentences[cut].span.start() - start;
        const bool balanced = at * 4 >= length && (length - at) * 4 >= length;
        const Millis pause =
            transcript.sentences[cut].span.start() - transcript.sentences[cut - 1].span.end();
        return std::tuple{balanced, balanced ? pause : Millis{0}, -std::chrono::abs(at - (length / 2))};
    };
    std::size_t best = first + 1;
    for (std::size_t cut = first + 2; cut < end; ++cut) {
        if (key(cut) > key(best)) {
            best = cut;
        }
    }
    return best;
}

// The cuts of [first, end), the longest runs cut again until none is longer
// than `longest` or is one sentence: the starts after `first`, in order.
void cut_stretch(const Transcript& transcript, std::size_t first, std::size_t end, Millis longest,
                 std::vector<std::size_t>& starts) {
    if (end - first < 2 || stretch_length(transcript, first, end) <= longest) {
        return;
    }
    const std::size_t cut = pause_cut(transcript, first, end);
    cut_stretch(transcript, first, cut, longest, starts);
    starts.push_back(cut);
    cut_stretch(transcript, cut, end, longest, starts);
}

std::string folded(std::string_view text) {
    return text::fold(text).value_or(std::string{text});
}

// The items of several briefs' lists (their speakers, or their words), each
// once by its folded `key`, with how many lists name it.
template <typename Item>
class Tally {
public:
    Tally(std::string Item::* key, std::optional<std::string> Item::* value) : key_(key), value_(value) {}

    // One brief's list: it names each item once, however often it lists it.
    void add(const std::vector<Item>& list) {
        std::set<std::size_t> named;
        for (const Item& item : list) {
            const auto [found, added] = index_.try_emplace(folded(item.*key_), entries_.size());
            if (added) {
                entries_.push_back(Entry{.item = item});
            }
            Entry& entry = entries_[found->second];
            if (!(entry.item.*value_).has_value()) {
                entry.item.*value_ = item.*value_;  // fills an empty one, never replaces one
            }
            if (named.insert(found->second).second) {
                ++entry.named;
            }
        }
    }

    // At most `most` items, those the most lists name (the first named on a
    // tie), in the order first named.
    [[nodiscard]] std::vector<Item> most_named(std::size_t most) const {
        std::vector<std::size_t> order(entries_.size());
        std::ranges::iota(order, std::size_t{0});
        std::ranges::stable_sort(order, std::greater{},
                                 [&](std::size_t index) { return entries_[index].named; });
        order.resize(std::min(most, order.size()));
        std::ranges::sort(order);
        std::vector<Item> items;
        items.reserve(order.size());
        for (const std::size_t index : order) {
            items.push_back(entries_[index].item);
        }
        return items;
    }

private:
    struct Entry {
        Item item;
        std::size_t named = 0;  // the lists that name it
    };

    std::string Item::* key_;
    std::optional<std::string> Item::* value_;
    std::vector<Entry> entries_;                  // in the order first named
    std::map<std::string, std::size_t> index_{};  // folded key → its entry
};

}  // namespace

Millis section_length(std::span<const Cut> cuts, std::size_t index, const Transcript& transcript) {
    return stretch_length(transcript, cuts[index].first, section_end(cuts, index, transcript));
}

std::vector<Cut> chapter_cuts(std::span<const media::Chapter> chapters, const Transcript& transcript) {
    std::vector<Cut> cuts;
    if (transcript.sentences.empty()) {
        return cuts;
    }
    for (const media::Chapter& chapter : chapters) {
        // The sentence that starts closest to the chapter's time; the earlier on a tie.
        const auto closest =
            std::ranges::min_element(transcript.sentences, {}, [&](const knowledge::Sentence& sentence) {
                return std::chrono::abs(sentence.span.start() - chapter.span.start());
            });
        const auto first =
            cuts.empty() ? std::size_t{0} : static_cast<std::size_t>(closest - transcript.sentences.begin());
        if (!cuts.empty() && first <= cuts.back().first) {
            continue;  // where, or before, the chapter before starts
        }
        cuts.push_back(Cut{.first = first, .title = chapter.title});
    }
    return cuts;
}

std::vector<Cut> cut_long(std::vector<Cut> cuts, const Transcript& transcript, Millis longest) {
    std::vector<Cut> parts;
    parts.reserve(cuts.size());
    for (std::size_t index = 0; index < cuts.size(); ++index) {
        const std::size_t end = section_end(cuts, index, transcript);
        std::vector<std::size_t> starts{cuts[index].first};
        cut_stretch(transcript, cuts[index].first, end, longest, starts);
        if (starts.size() == 1) {
            parts.push_back(std::move(cuts[index]));
            continue;
        }
        for (std::size_t part = 0; part < starts.size(); ++part) {
            parts.push_back(
                Cut{.first = starts[part],
                    .title = std::format("{} ({} of {})", cuts[index].title, part + 1, starts.size())});
        }
    }
    return parts;
}

std::vector<Cut> pause_cuts(const Transcript& transcript, std::size_t first, std::size_t end,
                            Millis longest) {
    const bool of_the_transcript = first <= end && end <= transcript.sentences.size();
    RR_ASSERT(of_the_transcript, "a stretch is of the transcript's sentences");
    std::vector<Cut> cuts;
    if (first == end) {
        return cuts;
    }
    std::vector<std::size_t> starts{first};
    cut_stretch(transcript, first, end, longest, starts);
    cuts.reserve(starts.size());
    for (std::size_t index = 0; index < starts.size(); ++index) {
        const std::size_t last = (index + 1 < starts.size() ? starts[index + 1] : end) - 1;
        cuts.push_back(Cut{
            .first = starts[index],
            .title =
                std::format("{}\u2013{}", format_timestamp(transcript.sentences[starts[index]].span.start()),
                            format_timestamp(transcript.sentences[last].span.end()))});
    }
    return cuts;
}

std::vector<Cut> agreed_cuts(std::span<const std::vector<Cut>> outlines, int tolerance, double quorum) {
    std::vector<std::vector<int>> starts;
    starts.reserve(outlines.size());
    for (const std::vector<Cut>& outline : outlines) {
        std::vector<int>& positions = starts.emplace_back();
        positions.reserve(outline.size());
        for (const Cut& cut : outline) {
            positions.push_back(static_cast<int>(cut.first));
        }
    }
    std::vector<Cut> agreed;
    for (const int position : reason::boundary_consensus(starts, tolerance, quorum)) {
        // The consensus keeps a position some outline proposed.
        for (const std::vector<Cut>& outline : outlines) {
            const auto found = std::ranges::find(outline, static_cast<std::size_t>(position), &Cut::first);
            if (found != outline.end()) {
                agreed.push_back(*found);
                break;
            }
        }
    }
    return agreed;
}

knowledge::Brief join_briefs(std::span<const knowledge::Brief> briefs) {
    knowledge::Brief joined;
    std::map<knowledge::Genre, std::size_t> votes;
    for (const knowledge::Brief& brief : briefs) {
        ++votes[brief.genre];
    }
    std::size_t most = 0;
    for (const knowledge::Brief& brief : briefs) {  // in order: the earliest wins a tie
        if (votes[brief.genre] > most) {
            most = votes[brief.genre];
            joined.genre = brief.genre;
        }
    }
    Tally<knowledge::Speaker> speakers{&knowledge::Speaker::name, &knowledge::Speaker::role};
    Tally<knowledge::LexiconTerm> lexicon{&knowledge::LexiconTerm::term, &knowledge::LexiconTerm::meaning};
    for (const knowledge::Brief& brief : briefs) {
        speakers.add(brief.speakers);
        lexicon.add(brief.lexicon);
    }
    joined.speakers = speakers.most_named(kMostSpeakers);
    joined.lexicon = lexicon.most_named(kMostTerms);
    return joined;
}

knowledge::Outline outline_of(std::span<const Cut> cuts, const Transcript& transcript, knowledge::Brief brief,
                              knowledge::SectionsFrom from) {
    const bool from_the_start = !cuts.empty() && cuts.front().first == 0;
    RR_ASSERT(from_the_start, "an outline's first section starts at the first sentence");
    knowledge::Outline outline{.brief = std::move(brief), .sections_from = from};
    outline.sections.reserve(cuts.size());
    for (std::size_t index = 0; index < cuts.size(); ++index) {
        const std::size_t end = section_end(cuts, index, transcript);
        RR_ASSERT(cuts[index].first < end, "cuts are in order, each before the next");
        outline.sections.push_back(knowledge::Section{.first = transcript.sentences[cuts[index].first].id,
                                                      .last = transcript.sentences[end - 1].id,
                                                      .title = cuts[index].title});
    }
    return outline;
}

}  // namespace rr::pipeline
