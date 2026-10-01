// WebVTT (formats.hpp).
//
//   WEBVTT
//
//   NOTE a comment
//
//   intro
//   00:00:01.000 --> 00:00:04.000 align:start
//   <v Ada>Hello, <i>everyone</i>.</v>
//
// After the header, blocks are separated by empty lines: comments ("NOTE"),
// styles, regions and cues. A cue has an optional identifier, its timing and
// its text, whose tags are markup: a voice (<v Ada>) names the speaker, a
// timestamp (<00:00:02.500>) times the word after it, and the others style.
//
// YouTube's automatic captions roll. Each cue shows the line before it again
// above its new words, and a 10 ms cue holds the line between them:
//
//   00:00:02.879 --> 00:00:05.990
//   hello and welcome
//   to<00:00:03.120><c> the</c><00:00:03.240><c> show</c>
//
// A file with timestamps in its cues is read as rolling: a line without a
// timestamp that repeats the line read before it is a repeat, read once.
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

struct Block {
    std::size_t first_line = 0;  // 1-based, for messages
    std::vector<std::string_view> lines{};
};

// The blocks of `lines`, which empty lines separate.
std::vector<Block> blocks_of(const std::vector<std::string_view>& lines) {
    std::vector<Block> blocks;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (lines[index].empty()) {
            continue;
        }
        if (index == 0 || lines[index - 1].empty()) {
            blocks.push_back(Block{.first_line = index + 1});
        }
        blocks.back().lines.push_back(lines[index]);
    }
    return blocks;
}

// Whether `line` starts with `keyword` alone, or followed by a space or a tab.
bool starts_block(std::string_view line, std::string_view keyword) {
    return line.starts_with(keyword) &&
           (line.size() == keyword.size() || line[keyword.size()] == ' ' || line[keyword.size()] == '\t');
}

// `text` without the spaces and tabs around it.
std::string_view trimmed(std::string_view text) {
    const std::size_t first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

// Whether a cue's line holds a timestamp tag: "<00:00:02.500>".
bool timed(std::string_view line) {
    for (std::size_t at = line.find('<'); at != std::string_view::npos; at = line.find('<', at + 1)) {
        if (at + 1 < line.size() && line[at + 1] >= '0' && line[at + 1] <= '9') {
            return true;
        }
    }
    return false;
}

// A cue's lines, read into its text, marks and voices.
class Payload {
public:
    explicit Payload(cues::Cue cue = {}) : cue_(std::move(cue)) {}

    void add_line(std::string_view line) {
        if (!cue_.text.empty()) {
            cue_.text.push_back('\n');
        }
        std::size_t at = 0;
        while (at < line.size()) {
            if (line[at] != '<') {
                const std::size_t tag = line.find('<', at);
                cue_.text += cues::decode_references(line.substr(at, tag - at));
                at = tag == std::string_view::npos ? line.size() : tag;
                continue;
            }
            const std::size_t close = line.find('>', at);
            if (close == std::string_view::npos) {
                break;  // a tag never closed runs to the end
            }
            tag(line.substr(at + 1, close - at - 1));
            at = close + 1;
        }
    }

    [[nodiscard]] const std::string& text() const { return cue_.text; }

    // The cue read: a voice still open ends with it.
    [[nodiscard]] cues::Cue finish() && {
        close_voice();
        return std::move(cue_);
    }

private:
    void tag(std::string_view tag) {
        if (!tag.empty() && tag.front() >= '0' && tag.front() <= '9') {
            if (const std::optional<Millis> time = timing(tag)) {
                cue_.marks.push_back(cues::Mark{.offset = cue_.text.size(), .time = *time});
            }
        } else if (tag == "v" || tag.starts_with("v ") || tag.starts_with("v.") || tag.starts_with("v\t")) {
            close_voice();
            const std::size_t space = tag.find_first_of(" \t");
            const std::string_view name =
                trimmed(space == std::string_view::npos ? "" : tag.substr(space + 1));
            if (!name.empty()) {
                cue_.voices.push_back(cues::Voice{.begin = cue_.text.size(),
                                                  .end = cue_.text.size(),
                                                  .name = cues::decode_references(name)});
                open_ = true;
            }
        } else if (tag == "/v") {
            close_voice();
        }
        // The rest style the text: <c.yellow>, <i>, <b>, <u>, <ruby>, <lang en>.
    }

    void close_voice() {
        if (open_) {
            cue_.voices.back().end = cue_.text.size();
            open_ = false;
        }
    }

    cues::Cue cue_;
    bool open_ = false;
};

// The cue of `block`: an identifier perhaps, its timing, then its text. In
// rolling captions, a line repeating the one read before (`last_line`) is
// left out.
Result<cues::Cue> cue_of(const Block& block, bool rolling, std::string& last_line) {
    const std::size_t timing_line = block.lines.front().contains(kArrow) ? 0 : 1;
    if (timing_line >= block.lines.size() || !block.lines[timing_line].contains(kArrow)) {
        return fail(Errc::invalid_argument,
                    std::format(R"(line {}: a cue without its timing ("00:00:01.000 --> 00:00:04.000"))",
                                block.first_line + timing_line));
    }
    const std::string_view written = block.lines[timing_line];
    const std::size_t arrow = written.find(kArrow);
    const std::string_view after = trimmed(written.substr(arrow + kArrow.size()));
    const std::optional<Millis> start = timing(trimmed(written.substr(0, arrow)));
    const std::optional<Millis> end = timing(after.substr(0, after.find_first_of(" \t")));  // settings follow
    if (!start.has_value() || !end.has_value()) {
        return fail(Errc::invalid_argument, std::format(R"(line {}: "{}" is no cue timing)",
                                                        block.first_line + timing_line, written));
    }
    Payload payload{cues::Cue{.start = *start, .end = *end}};
    for (std::size_t index = timing_line + 1; index < block.lines.size(); ++index) {
        const std::string_view line = block.lines[index];
        if (rolling) {
            Payload alone;
            alone.add_line(line);
            const std::string_view text = trimmed(alone.text());
            if (!timed(line) && (text.empty() || text == last_line)) {
                continue;  // a repeat of the line before, or a blank
            }
            last_line = text;
        }
        payload.add_line(line);
    }
    return std::move(payload).finish();
}

}  // namespace

Result<std::vector<cues::Cue>> vtt(std::string_view content) {
    const std::vector<std::string_view> lines = lines_of(content);
    if (lines.empty() || !starts_block(lines.front(), "WEBVTT")) {
        return fail(Errc::invalid_argument, "not WebVTT: its first line is not \"WEBVTT\"");
    }
    std::vector<Block> blocks = blocks_of(lines);
    // The header ("WEBVTT" and its metadata) ends at the first empty line. A
    // file that runs on into its first cue keeps that cue.
    Block& header = blocks.front();
    const auto timing_in_header =
        std::ranges::find_if(header.lines, [](std::string_view line) { return line.contains(kArrow); });
    if (timing_in_header == header.lines.end()) {
        blocks.erase(blocks.begin());
    } else {
        header.first_line += static_cast<std::size_t>(timing_in_header - header.lines.begin());
        header.lines.erase(header.lines.begin(), timing_in_header);
    }
    const bool rolling = std::ranges::any_of(
        blocks, [](const Block& block) { return std::ranges::any_of(block.lines, timed); });
    std::vector<cues::Cue> read;
    std::string last_line;  // the text of the last line read, when rolling
    for (const Block& block : blocks) {
        const std::string_view first = block.lines.front();
        if (starts_block(first, "NOTE") || starts_block(first, "STYLE") || starts_block(first, "REGION")) {
            continue;
        }
        RR_TRY_ASSIGN(cues::Cue cue, cue_of(block, rolling, last_line));
        read.push_back(std::move(cue));
    }
    return read;
}

}  // namespace rr::transcript::formats
