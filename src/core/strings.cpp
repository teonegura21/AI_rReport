#include <rr/core/strings.hpp>

#include <algorithm>

#include <rr/core/assert.hpp>

namespace rr {
namespace {

constexpr bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

constexpr char lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr char upper(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

template <class Part>
std::string join_impl(std::span<const Part> parts, std::string_view separator) {
    std::string result;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index > 0) {
            result += separator;
        }
        result += parts[index];
    }
    return result;
}

}  // namespace

std::string_view trim(std::string_view text) noexcept {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && is_space(text[begin])) {
        ++begin;
    }
    while (end > begin && is_space(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::vector<std::string_view> split(std::string_view text, char delimiter) {
    std::vector<std::string_view> parts;
    std::size_t begin = 0;
    while (true) {
        const std::size_t position = text.find(delimiter, begin);
        if (position == std::string_view::npos) {
            parts.push_back(text.substr(begin));
            return parts;
        }
        parts.push_back(text.substr(begin, position - begin));
        begin = position + 1;
    }
}

std::string join(std::span<const std::string> parts, std::string_view separator) {
    return join_impl(parts, separator);
}

std::string join(std::span<const std::string_view> parts, std::string_view separator) {
    return join_impl(parts, separator);
}

std::string to_lower_ascii(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), lower);
    return result;
}

std::string to_upper_ascii(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), upper);
    return result;
}

bool iequals_ascii(std::string_view lhs, std::string_view rhs) noexcept {
    return std::ranges::equal(lhs, rhs, [](char a, char b) { return lower(a) == lower(b); });
}

std::string replace_all(std::string_view text, std::string_view from, std::string_view to) {
    RR_ASSERT(!from.empty(), "replace_all: the pattern must not be empty");
    std::string result;
    result.reserve(text.size());
    std::size_t begin = 0;
    while (true) {
        const std::size_t position = text.find(from, begin);
        if (position == std::string_view::npos) {
            result.append(text.substr(begin));
            return result;
        }
        result.append(text.substr(begin, position - begin));
        result.append(to);
        begin = position + from.size();
    }
}

}  // namespace rr
