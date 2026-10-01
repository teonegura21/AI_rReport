// The marker parser's fuzz target (ADR-0006: the parser is correctness- and
// security-critical). Whatever a writer model emits, for every input:
//   * blocks are in order, apart and within the article; a block's sentences
//     lie within it, in order and apart; code has none;
//   * every marker reads exactly "[A<n>]" for its own atom, lies within its
//     sentence, and markers are in order and apart;
//   * bad markers lie within the article and never overlap a marker;
//   * cited() lists each cited atom once;
//   * parsing is deterministic, and without_markers() only removes.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <rr/knowledge/article.hpp>

namespace rr::knowledge {
namespace {

void require(bool holds, const char* invariant) {
    if (!holds) {
        (void)std::fprintf(stderr, "broken invariant: %s\n", invariant);
        std::abort();
    }
}

void check_blocks(std::string_view markdown, const ParsedArticle& parsed, std::vector<text::Span>& markers) {
    std::size_t previous_block_end = 0;
    for (const Block& block : parsed.blocks) {
        require(block.span.begin >= previous_block_end && block.span.begin <= block.span.end &&
                    block.span.end <= markdown.size(),
                "blocks are in order, apart and within the article");
        require(block.kind != BlockKind::code || block.sentences.empty(), "code holds no sentences");
        previous_block_end = block.span.end;
        std::size_t previous_sentence_end = block.span.begin;
        for (const CitingSentence& sentence : block.sentences) {
            require(sentence.span.begin >= previous_sentence_end && sentence.span.begin < sentence.span.end &&
                        sentence.span.end <= block.span.end,
                    "a block's sentences are within it, in order and apart");
            previous_sentence_end = sentence.span.end;
            std::size_t previous_marker_end = sentence.span.begin;
            for (const Marker& marker : sentence.markers) {
                require(marker.span.begin >= previous_marker_end && marker.span.end <= sentence.span.end,
                        "markers are within their sentence, in order and apart");
                const std::string_view written = text::text_of(markdown, marker.span);
                require(written.size() >= 3 && written.front() == '[' && written.back() == ']' &&
                            AtomId::parse(written.substr(1, written.size() - 2)) == marker.atom,
                        "a marker reads exactly [A<n>] for its own atom");
                previous_marker_end = marker.span.end;
                markers.push_back(marker.span);
            }
        }
    }
}

void check_bad_markers(std::string_view markdown, const ParsedArticle& parsed,
                       const std::vector<text::Span>& markers) {
    for (const BadMarker& bad : parsed.bad_markers) {
        require(bad.span.begin < bad.span.end && bad.span.end <= markdown.size() && !bad.reason.empty(),
                "a bad marker is within the article, with a reason");
        for (const text::Span& marker : markers) {
            require(bad.span.end <= marker.begin || marker.end <= bad.span.begin,
                    "a bad marker overlaps no marker");
        }
    }
}

void check(std::string_view markdown) {
    const ParsedArticle parsed = parse_article(markdown);
    std::vector<text::Span> markers;
    check_blocks(markdown, parsed, markers);
    check_bad_markers(markdown, parsed, markers);

    const std::vector<AtomId> cited = parsed.cited();
    require(std::unordered_set<AtomId>(cited.begin(), cited.end()).size() == cited.size(),
            "cited() lists each once");
    require(parse_article(markdown) == parsed, "parsing is deterministic");
    const std::string seen =
        without_markers(markdown, parsed, text::Span{.begin = 0, .end = markdown.size()});
    require(seen.size() <= markdown.size(), "without_markers() only removes");
}

}  // namespace
}  // namespace rr::knowledge

// libFuzzer's entry point: its name is libFuzzer's.
extern "C" int LLVMFuzzerTestOneInput(  // NOLINT(readability-identifier-naming)
    const std::uint8_t* data, std::size_t size) {
    rr::knowledge::check(std::string_view{reinterpret_cast<const char*>(data), size});
    return 0;
}
