// Test helpers for rr_text: what spans cover, as strings, so that
// expectations read as text and failures show it.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <rr/text/span.hpp>

namespace rr::text {

inline std::vector<std::string> covered(std::string_view text, const std::vector<Span>& spans) {
    std::vector<std::string> found;
    found.reserve(spans.size());
    for (const Span& span : spans) {
        found.emplace_back(text_of(text, span));
    }
    return found;
}

}  // namespace rr::text
