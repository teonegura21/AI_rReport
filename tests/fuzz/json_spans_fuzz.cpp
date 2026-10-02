// json::spans()' fuzz target, differential against parse(). A model writes
// the text, and confidence is read from where its values stand (P28). For
// every input:
//   * spans() refuses exactly what parse() refuses, with parse()'s message;
//   * on JSON, spans are in the order they begin, each within the text and
//     within the span of its parent (the pointer without its last token);
//   * each span's text parses to the value parse() puts at its pointer,
//     one span a pointer, and every value of the document has one.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <rr/json/json.hpp>
#include <rr/json/spans.hpp>

namespace rr::json {
namespace {

void require(bool holds, const char* invariant) {
    if (!holds) {
        (void)std::fprintf(stderr, "broken invariant: %s\n", invariant);
        std::abort();
    }
}

std::size_t values_in(const Json& document) {
    std::size_t count = 0;
    std::vector<const Json*> left{&document};  // a stack, not recursion: any depth
    while (!left.empty()) {
        const Json* value = left.back();
        left.pop_back();
        ++count;
        if (value->is_structured()) {
            for (const Json& child : *value) {
                left.push_back(&child);
            }
        }
    }
    return count;
}

std::string parent_of(const std::string& pointer) {
    return pointer.substr(0, pointer.rfind('/'));
}

void check(std::string_view text) {
    const Result<Json> parsed = parse(text);
    const Result<std::vector<Span>> found = spans(text);
    require(parsed.has_value() == found.has_value(), "spans() refuses exactly what parse() refuses");
    if (!parsed.has_value()) {
        require(found.error().message() == parsed.error().message(), "with parse()'s message");
        return;
    }
    std::map<std::string, const Span*> by_pointer;
    std::size_t previous_begin = 0;
    for (const Span& span : *found) {
        require(span.begin >= previous_begin && span.begin < span.end && span.end <= text.size(),
                "spans are in the order they begin, each within the text");
        previous_begin = span.begin;
        if (!span.pointer.empty()) {
            const auto parent = by_pointer.find(parent_of(span.pointer));
            require(parent != by_pointer.end() && parent->second->begin < span.begin &&
                        span.end < parent->second->end,
                    "a value lies within its parent");
        }
        require(by_pointer.emplace(span.pointer, &span).second, "one span a pointer");
        const Json::json_pointer at{span.pointer};
        require(parsed->contains(at), "every span names a value parse() keeps");
        const Result<Json> value = parse(text.substr(span.begin, span.end - span.begin));
        require(value.has_value() && *value == parsed->at(at), "a span's text is its value");
    }
    require(found->size() == values_in(*parsed), "every value has a span");
}

}  // namespace
}  // namespace rr::json

extern "C" int LLVMFuzzerTestOneInput(  // NOLINT(readability-identifier-naming)
    const std::uint8_t* data, std::size_t size) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): libFuzzer's bytes, read as text
    rr::json::check(std::string_view{reinterpret_cast<const char*>(data), size});
    return 0;
}
