// rr/proc/line_splitter.hpp — turns a byte stream into lines.
//
// Why this exists
//   Child output arrives in arbitrary chunks: a line can span several reads
//   and one read can hold many lines. Consumers (progress parsers, the log)
//   want whole lines. Three details matter:
//     * '\n', '\r' and "\r\n" all end a line. Progress bars (ffmpeg, yt-dlp)
//       redraw one line with '\r'; treating it as a line end turns every
//       redraw into an update we can parse.
//     * Empty lines are skipped. This makes "\r\n" a single line end, and
//       blank lines carry nothing a parser or a log needs.
//     * A line longer than `max_line` bytes is emitted in pieces, so a child
//       that never prints a newline cannot grow our memory without bound.
//
// Thread-safety: none; one splitter per stream, used by one thread.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace rr::proc {

using LineCallback = std::function<void(std::string_view line)>;

class LineSplitter {
public:
    static constexpr std::size_t kDefaultMaxLine = std::size_t{64} << 10U;  // 64 KiB

    explicit LineSplitter(LineCallback on_line, std::size_t max_line = kDefaultMaxLine);

    // Emits every line completed by `bytes`; keeps an unfinished tail.
    void feed(std::string_view bytes);
    // Emits the unfinished tail, if any (the stream ended without a newline).
    void finish();

private:
    void append(std::string_view text);
    void emit();

    LineCallback on_line_;
    std::size_t max_line_;
    std::string partial_;
};

}  // namespace rr::proc
