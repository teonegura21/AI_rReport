#include <rr/proc/line_splitter.hpp>

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace rr::proc {
namespace {

std::vector<std::string> split(const std::vector<std::string_view>& chunks, std::size_t max_line = 1000) {
    std::vector<std::string> lines;
    LineSplitter splitter{[&](std::string_view line) { lines.emplace_back(line); }, max_line};
    for (const auto chunk : chunks) {
        splitter.feed(chunk);
    }
    splitter.finish();
    return lines;
}

using Lines = std::vector<std::string>;

TEST(LineSplitter, SplitsOnEveryLineEnding) {
    EXPECT_EQ(split({"unix\nwindows\r\nprogress 10%\rprogress 20%\r"}),
              (Lines{"unix", "windows", "progress 10%", "progress 20%"}));
}

TEST(LineSplitter, JoinsLinesThatSpanChunks) {
    EXPECT_EQ(split({"hel", "lo wo", "rld\nsec", "ond\n"}), (Lines{"hello world", "second"}));
    EXPECT_EQ(split({"line\r", "\nnext\n"}), (Lines{"line", "next"})) << "\\r\\n split across two reads";
}

TEST(LineSplitter, SkipsEmptyLines) {
    EXPECT_EQ(split({"\n\na\n\r\n\rb\n\n"}), (Lines{"a", "b"}));
}

TEST(LineSplitter, FinishEmitsAnUnterminatedTail) {
    EXPECT_EQ(split({"complete\npartial"}), (Lines{"complete", "partial"}));
    EXPECT_TRUE(split({}).empty());
}

TEST(LineSplitter, CutsOverlongLinesIntoPieces) {
    EXPECT_EQ(split({"abcdefgh", "ij\nk"}, 4), (Lines{"abcd", "efgh", "ij", "k"}));
}

TEST(LineSplitter, WithoutACallbackLinesAreDropped) {
    LineSplitter splitter{nullptr};
    splitter.feed("ignored\n");
    splitter.finish();
    SUCCEED();
}

}  // namespace
}  // namespace rr::proc
