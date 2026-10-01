#include <rr/transcript/captions.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cues.hpp"
#include "formats.hpp"

namespace rr::transcript {

namespace formats {

std::vector<std::string_view> lines_of(std::string_view content) {
    if (content.starts_with("\xEF\xBB\xBF")) {
        content.remove_prefix(3);  // the byte order mark of UTF-8
    }
    std::vector<std::string_view> lines;
    std::size_t begin = 0;
    for (std::size_t at = 0; at < content.size(); ++at) {
        if (content[at] != '\n' && content[at] != '\r') {
            continue;
        }
        lines.push_back(content.substr(begin, at - begin));
        if (content[at] == '\r' && at + 1 < content.size() && content[at + 1] == '\n') {
            ++at;
        }
        begin = at + 1;
    }
    if (begin < content.size()) {
        lines.push_back(content.substr(begin));
    }
    return lines;
}

std::optional<Millis> timing(std::string_view text) {
    constexpr std::size_t kFraction = 3;
    const std::size_t point = text.find_first_of(".,");
    // A timing has minutes and seconds at least, and a fraction of them.
    const bool shaped =
        point != std::string_view::npos && text.find(':') < point && text.size() - point - 1 == kFraction;
    if (!shaped) {
        return std::nullopt;
    }
    std::string written{text};
    written[point] = '.';
    const Result<Millis> parsed = parse_timestamp(written);
    return parsed.has_value() && *parsed <= kLatest ? std::optional<Millis>{*parsed} : std::nullopt;
}

// Plain text: paragraphs, which blank lines separate, each a cue at 0.
std::vector<cues::Cue> text(std::string_view content) {
    std::vector<cues::Cue> read;
    bool in_paragraph = false;
    for (const std::string_view line : lines_of(content)) {
        if (line.find_first_not_of(" \t") == std::string_view::npos) {
            in_paragraph = false;
            continue;
        }
        if (!in_paragraph) {
            read.push_back(cues::Cue{.paragraph = true});
            in_paragraph = true;
        } else {
            read.back().text.push_back('\n');
        }
        read.back().text += line;
    }
    return read;
}

}  // namespace formats

std::string_view to_string(CaptionFormat format) noexcept {
    switch (format) {
        case CaptionFormat::json3:
            return "json3";
        case CaptionFormat::vtt:
            return "vtt";
        case CaptionFormat::srt:
            return "srt";
        case CaptionFormat::text:
            return "text";
    }
    return "unknown";
}

std::optional<CaptionFormat> format_of(std::string_view file_name) {
    const std::size_t dot = file_name.rfind('.');
    if (dot == std::string_view::npos) {
        return std::nullopt;
    }
    std::string extension{file_name.substr(dot + 1)};
    for (char& c : extension) {
        c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    for (const CaptionFormat format :
         {CaptionFormat::json3, CaptionFormat::vtt, CaptionFormat::srt, CaptionFormat::text}) {
        if (extension == (format == CaptionFormat::text ? "txt" : to_string(format))) {
            return format;
        }
    }
    return std::nullopt;
}

Result<CaptionTrack> read_captions(std::string_view content, CaptionFormat format) {
    std::vector<cues::Cue> read;
    switch (format) {
        case CaptionFormat::json3: {
            RR_TRY_ASSIGN(read, formats::json3(content));
            break;
        }
        case CaptionFormat::vtt: {
            RR_TRY_ASSIGN(read, formats::vtt(content));
            break;
        }
        case CaptionFormat::srt: {
            RR_TRY_ASSIGN(read, formats::srt(content));
            break;
        }
        case CaptionFormat::text:
            read = formats::text(content);
            break;
    }
    CaptionTrack track{.timed = format != CaptionFormat::text};
    cues::add_cues(std::move(read), track);
    return track;
}

}  // namespace rr::transcript
