// Automatic captions, punctuated a window at a time (rr/pipeline/punctuation.hpp).
#include <rr/pipeline/punctuation.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>

#include <rr/core/assert.hpp>
#include <rr/transcript/projection.hpp>
#include <rr/transcript/sentences.hpp>

namespace rr::pipeline {

namespace {

using transcript::Word;

// The bytes of words [begin, end) joined by single spaces, from the prefix
// sums `ends`, where ends[i] is the bytes of words [0, i), each followed by a
// space.
std::size_t bytes(const std::vector<std::size_t>& ends, std::size_t begin, std::size_t end) {
    return end > begin ? ends[end] - ends[begin] - 1 : 0;
}

// How long the speaker paused before word `index`.
Millis pause_before(const std::vector<Word>& words, std::size_t index) {
    return words[index].start - words[index - 1].end;
}

// Where the window starting at `begin` ends (see the header comment).
std::size_t cut_after(const std::vector<Word>& words, const std::vector<std::size_t>& ends, std::size_t begin,
                      const WindowSize& size) {
    std::optional<std::size_t> best;
    std::size_t last_fitting = begin + 1;  // a word too long for any window is one of its own
    for (std::size_t end = begin + 1; end < words.size(); ++end) {
        const std::size_t length = bytes(ends, begin, end);
        if (length > size.most) {
            break;
        }
        last_fitting = end;
        if (length < size.least) {
            continue;
        }
        const auto distance = [&](std::size_t cut) {
            const std::size_t at = bytes(ends, begin, cut);
            return at > size.target ? at - size.target : size.target - at;
        };
        const bool longer = best.has_value() && pause_before(words, end) > pause_before(words, *best);
        const bool as_long_and_nearer = best.has_value() &&
                                        pause_before(words, end) == pause_before(words, *best) &&
                                        distance(end) < distance(*best);
        if (!best.has_value() || longer || as_long_and_nearer) {
            best = end;
        }
    }
    // No cut leaves `least` bytes or more without passing `most`: a word too
    // long to share a window jumps over the range.
    return best.value_or(last_fitting);
}

// The answer's projection onto the excerpt's words.
Result<transcript::Projection> projected(const std::vector<Word>& words, const Window& window,
                                         std::string_view answer) {
    const auto first = words.begin() + static_cast<std::ptrdiff_t>(window.context_begin);
    const auto last = words.begin() + static_cast<std::ptrdiff_t>(window.context_end);
    return transcript::project(std::vector<Word>(first, last), answer);
}

}  // namespace

std::vector<Window> windows_of(const std::vector<Word>& words, const WindowSize& size) {
    const bool ordered = size.least <= size.target && size.target <= size.most;
    RR_ASSERT(ordered, "a window's sizes are in order");
    std::vector<std::size_t> ends{0};
    ends.reserve(words.size() + 1);
    for (const Word& word : words) {
        ends.push_back(ends.back() + word.text.size() + 1);
    }
    std::vector<Window> windows;
    for (std::size_t begin = 0; begin < words.size();) {
        const std::size_t end = bytes(ends, begin, words.size()) <= size.most
                                    ? words.size()
                                    : cut_after(words, ends, begin, size);
        Window window{.begin = begin, .end = end, .context_begin = begin, .context_end = end};
        while (window.context_begin > 0 && bytes(ends, window.context_begin - 1, begin) <= size.context) {
            --window.context_begin;
        }
        while (window.context_end < words.size() &&
               bytes(ends, end, window.context_end + 1) <= size.context) {
            ++window.context_end;
        }
        windows.push_back(window);
        begin = end;
    }
    return windows;
}

std::string excerpt_of(const std::vector<Word>& words, const Window& window) {
    std::string excerpt;
    for (std::size_t index = window.context_begin; index < window.context_end; ++index) {
        if (index > window.context_begin) {
            excerpt += ' ';
        }
        excerpt += words[index].text;
    }
    return excerpt;
}

std::vector<std::string> punctuation_problems(const std::vector<Word>& words, const Window& window,
                                              std::string_view answer) {
    const Result<transcript::Projection> projection = projected(words, window, answer);
    if (!projection.has_value()) {
        return {projection.error().message()};
    }
    std::vector<std::string> problems;
    const std::size_t count = window.context_end - window.context_begin;
    const std::size_t differ = projection->changed + projection->added + projection->dropped;
    if (differ > std::max<std::size_t>(2, count / 20)) {
        problems.push_back(
            std::format("Keep every word exactly as it is, in the same order, fillers and repetitions too: "
                        "{} of the {} words "
                        "were changed, added or left out. Add only punctuation and capital letters.",
                        differ, count));
    }
    if (!transcript::punctuated(projection->words)) {
        problems.emplace_back(
            "Mark where each sentence ends, with \".\", \"?\" or \"!\": the text still reads as one long "
            "sentence.");
    }
    return problems;
}

Result<std::vector<Word>> punctuated_words(const std::vector<Word>& words, const Window& window,
                                           std::string_view answer) {
    RR_TRY_ASSIGN(transcript::Projection projection, projected(words, window, answer));
    const auto first =
        projection.words.begin() + static_cast<std::ptrdiff_t>(window.begin - window.context_begin);
    const auto last =
        projection.words.begin() + static_cast<std::ptrdiff_t>(window.end - window.context_begin);
    return std::vector<Word>(std::make_move_iterator(first), std::make_move_iterator(last));
}

}  // namespace rr::pipeline
