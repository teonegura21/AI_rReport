// The caption readers' fuzz target. A caption file comes from anywhere: from
// YouTube through yt-dlp, or from the user's disk. The first byte of an input
// picks its format, the rest is the file. Whatever it holds:
//   * reading it never crashes. When it reads, its words are non-empty and
//     hold no blank (a space or a control, as rr/text reads them) but a joiner
//     within. They are valid UTF-8 in NFC, and their times are in order:
//     starts never decrease, and each word ends no earlier than it starts and
//     no later than the next starts;
//   * the transcript cut from the words is numbered S1, S2, …, its spans start
//     in order and end after they start, and it loses no character of the
//     words and adds none;
//   * projecting the file itself onto its own words, as a model's text,
//     changes no word, keeps every time and adds no blank (the projection
//     checks its words itself).
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <rr/text/unicode.hpp>
#include <rr/transcript/captions.hpp>
#include <rr/transcript/projection.hpp>
#include <rr/transcript/sentences.hpp>

namespace rr::transcript {
namespace {

void require(bool holds, const char* invariant) {
    if (!holds) {
        (void)std::fprintf(stderr, "broken invariant: %s\n", invariant);
        std::abort();
    }
}

// A space, or what rr/text never reads as text: a control, a format mark, an
// unassigned code point.
bool is_blank(char32_t code_point) {
    const text::CharClass kind = text::classify(code_point);
    return kind == text::CharClass::space || kind == text::CharClass::other;
}

// Whether `word` holds no blank, but a joiner (U+200C, U+200D) between two of
// its characters.
bool well_formed(std::string_view word) {
    const std::vector<text::CodePoint> points = text::code_points(word);
    for (std::size_t index = 0; index < points.size(); ++index) {
        const char32_t value = points[index].value;
        const bool joiner =
            (value == U'\u200C' || value == U'\u200D') && index > 0 && index + 1 < points.size();
        if (is_blank(value) && !joiner) {
            return false;
        }
    }
    return !points.empty();
}

// What is not blank: the characters a transcript must keep, in order.
std::string unspaced(std::string_view text) {
    std::string kept;
    for (const text::CodePoint& point : text::code_points(text)) {
        if (!is_blank(point.value)) {
            kept.append(text.substr(point.begin, point.end - point.begin));
        }
    }
    return kept;
}

void check_words(const std::vector<Word>& words) {
    for (std::size_t index = 0; index < words.size(); ++index) {
        const Word& word = words[index];
        require(well_formed(word.text), "a word is non-empty, and holds no blank but a joiner within");
        require(text::valid_utf8(word.text) && text::nfc(word.text) == word.text,
                "a word is valid UTF-8 in NFC");
        require(word.start >= Millis{0} && word.end >= word.start, "a word ends no earlier than it starts");
        if (index + 1 < words.size()) {
            require(words[index + 1].start >= word.start, "starts never decrease");
            require(word.end <= words[index + 1].start, "a word ends no later than the next starts");
        }
    }
}

void check_transcript(const std::vector<Word>& words) {
    const knowledge::Transcript transcript = transcript_of(words, SentenceOptions{.max_words = 8});
    std::string said;
    for (const Word& word : words) {
        said += unspaced(word.text);
    }
    std::string kept;
    for (std::size_t index = 0; index < transcript.sentences.size(); ++index) {
        const knowledge::Sentence& sentence = transcript.sentences[index];
        require(sentence.id.number() == index + 1, "sentences are numbered S1, S2, \u2026");
        require(index == 0 || sentence.span.start() >= transcript.sentences[index - 1].span.start(),
                "sentences start in order");
        kept += unspaced(sentence.text);
    }
    require(kept == said, "the transcript loses no character of the words, and adds none");
}

void check_projection(const std::vector<Word>& words, std::string_view model) {
    constexpr std::size_t kMostWords = 300;  // the alignment's time grows with the product of the sizes
    if (words.size() > kMostWords || model.size() > kMostWords * 8) {
        return;
    }
    const Result<Projection> projection = project(words, model);
    if (!projection.has_value()) {
        return;
    }
    require(projection->words.size() == words.size(), "a projection has the captions' words, one for one");
    for (std::size_t index = 0; index < words.size(); ++index) {
        require(projection->words[index].start == words[index].start &&
                    projection->words[index].end == words[index].end,
                "a projection keeps every time");
        require(well_formed(projection->words[index].text), "a projection adds no blank");
    }
}

}  // namespace
}  // namespace rr::transcript

// libFuzzer's entry point: its name is libFuzzer's.
extern "C" int LLVMFuzzerTestOneInput(  // NOLINT(readability-identifier-naming)
    const std::uint8_t* data, std::size_t size) {
    using rr::transcript::CaptionFormat;
    if (size == 0) {
        return 0;
    }
    constexpr std::uint8_t kFormats = 4;
    const auto format = static_cast<CaptionFormat>(data[0] % kFormats);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): libFuzzer hands bytes; text is char.
    const std::string_view content(reinterpret_cast<const char*>(data + 1), size - 1);
    const rr::Result<rr::transcript::CaptionTrack> track = rr::transcript::read_captions(content, format);
    if (!track.has_value()) {
        return 0;
    }
    rr::transcript::check_words(track->words);
    rr::transcript::check_transcript(track->words);
    rr::transcript::check_projection(track->words, content);
    return 0;
}
