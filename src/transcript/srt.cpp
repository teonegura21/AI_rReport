// SubRip (formats.hpp).
//
//   1
//   00:00:01,000 --> 00:00:04,000
//   <i>Hello,</i> everyone.
//
// Blocks are separated by blank lines: a cue's number, its timing and its
// text, whose tags (<i>, <b>, <u>, <font color="...">) and override codes
// ({\an8}) style it. The number is optional here: some writers leave it out.
#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cues.hpp"
#include "formats.hpp"

namespace rr::transcript::formats {

namespace {

constexpr std::string_view kArrow = "-->";

bool blank(std::string_view line) {
    return line.find_first_not_of(" \t") == std::string_view::npos;
}

bool all_digits(std::string_view line) {
    return !line.empty() && line.find_first_not_of("0123456789") == std::string_view::npos;
}

bool is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// `line` without its styling: an HTML-like tag ("<i>", "</font>") or an
// override code ("{\an8}"). Other brackets are text: "<3", "{sic}".
std::string unstyled(std::string_view line) {
    std::string text;
    std::size_t at = 0;
    while (at < line.size()) {
        const char c = line[at];
        const bool tag = c == '<' && at + 1 < line.size() &&
                         (is_letter(line[at + 1]) ||
                          (line[at + 1] == '/' && at + 2 < line.size() && is_letter(line[at + 2])));
        const bool code = c == '{' && at + 1 < line.size() && line[at + 1] == '\\';
        std::size_t close = std::string_view::npos;
        if (tag) {
            close = line.find('>', at);
        } else if (code) {
            close = line.find('}', at);
        }
        if (close != std::string_view::npos) {
            at = close + 1;
            continue;
        }
        text.push_back(c);
        ++at;
    }
    return text;
}

// "00:00:01,000 --> 00:00:04,000", perhaps with a position after: the start
// and end, or nullopt.
std::optional<std::pair<Millis, Millis>> timings(std::string_view line) {
    const std::size_t arrow = line.find(kArrow);
    const auto trim = [](std::string_view text) {
        const std::size_t first = text.find_first_not_of(" \t");
        return first == std::string_view::npos ? std::string_view{}
                                               : text.substr(first, text.find_last_not_of(" \t") - first + 1);
    };
    const std::string_view after = trim(line.substr(arrow + kArrow.size()));
    const std::optional<Millis> start = timing(trim(line.substr(0, arrow)));
    const std::optional<Millis> end = timing(after.substr(0, after.find_first_of(" \t")));
    if (!start.has_value() || !end.has_value()) {
        return std::nullopt;
    }
    return std::pair{*start, *end};
}

}  // namespace

Result<std::vector<cues::Cue>> srt(std::string_view content) {
    const std::vector<std::string_view> lines = lines_of(content);
    std::vector<cues::Cue> read;
    std::size_t index = 0;
    while (index < lines.size()) {
        if (blank(lines[index])) {
            ++index;
            continue;
        }
        const std::size_t timing_line = all_digits(lines[index]) ? index + 1 : index;
        const std::optional<std::pair<Millis, Millis>> span =
            timing_line < lines.size() && lines[timing_line].contains(kArrow) ? timings(lines[timing_line])
                                                                              : std::nullopt;
        if (!span.has_value()) {
            const std::size_t shown = std::min(timing_line, lines.size() - 1);
            return fail(Errc::invalid_argument,
                        std::format(R"(line {}: "{}" is no cue timing ("00:00:01,000 --> 00:00:04,000"))",
                                    shown + 1, lines[shown]));
        }
        cues::Cue cue{.start = span->first, .end = span->second};
        for (index = timing_line + 1; index < lines.size() && !blank(lines[index]); ++index) {
            if (!cue.text.empty()) {
                cue.text.push_back('\n');
            }
            cue.text += cues::decode_references(unstyled(lines[index]));
        }
        read.push_back(std::move(cue));
    }
    return read;
}

}  // namespace rr::transcript::formats
