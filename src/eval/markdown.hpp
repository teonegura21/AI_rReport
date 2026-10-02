// markdown.hpp — what the eval reads of an article's Markdown (private to
// rr_eval): the judgement and the pairing both cut an article at its
// headings.
#pragma once

#include <string>
#include <string_view>

namespace rr::eval::detail {

// A heading's words, without the marks that make it one: "## Weighing radium"
// gives "Weighing radium".
[[nodiscard]] inline std::string heading_text(std::string_view line) {
    const std::size_t words = line.find_first_not_of("# \t");
    std::string_view text = words == std::string_view::npos ? std::string_view{} : line.substr(words);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return std::string{text};
}

}  // namespace rr::eval::detail
