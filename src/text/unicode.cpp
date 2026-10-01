#include <rr/text/unicode.hpp>

#include <cstdlib>
#include <format>
#include <memory>

#include <utf8proc.h>

#include <rr/core/strings.hpp>

namespace rr::text {

namespace {

constexpr std::string_view kReplacement = "\xEF\xBF\xBD";  // U+FFFD in UTF-8

// The code point at `text[at]` and its length in bytes; a negative length for
// an invalid sequence.
utf8proc_ssize_t next(std::string_view text, std::size_t at, utf8proc_int32_t& code_point) noexcept {
    return utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t*>(text.data() + at),
                            static_cast<utf8proc_ssize_t>(text.size() - at), &code_point);
}

// `text` mapped by utf8proc with `options`; `text` must be valid UTF-8.
Result<std::string> mapped(std::string_view text, int options) {
    if (!valid_utf8(text)) {
        return fail(Errc::invalid_argument, "the text is not valid UTF-8");
    }
    if (text.empty()) {
        return std::string{};
    }
    utf8proc_uint8_t* output = nullptr;
    // utf8proc's options are bit flags: their combinations are no enumerators.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto flags = static_cast<utf8proc_option_t>(options);
    const utf8proc_ssize_t length = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(text.data()),
                                                 static_cast<utf8proc_ssize_t>(text.size()), &output, flags);
    const std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)> owned{output, &std::free};
    if (length < 0) {
        return fail(Errc::invalid_argument,
                    std::format("the text cannot be normalized: {}", utf8proc_errmsg(length)));
    }
    return std::string{reinterpret_cast<const char*>(output), static_cast<std::size_t>(length)};
}

}  // namespace

bool valid_utf8(std::string_view text) noexcept {
    for (std::size_t at = 0; at < text.size();) {
        utf8proc_int32_t code_point = 0;
        const utf8proc_ssize_t length = next(text, at, code_point);
        if (length <= 0) {
            return false;
        }
        at += static_cast<std::size_t>(length);
    }
    return true;
}

std::string repair(std::string_view text) {
    std::string repaired;
    repaired.reserve(text.size());
    for (std::size_t at = 0; at < text.size();) {
        utf8proc_int32_t code_point = 0;
        const utf8proc_ssize_t length = next(text, at, code_point);
        if (length <= 0) {
            repaired += kReplacement;
            ++at;
            continue;
        }
        repaired.append(text.substr(at, static_cast<std::size_t>(length)));
        at += static_cast<std::size_t>(length);
    }
    return repaired;
}

Result<std::string> nfc(std::string_view text) {
    return mapped(text, UTF8PROC_STABLE | UTF8PROC_COMPOSE);
}

Result<std::string> fold(std::string_view text) {
    RR_TRY_ASSIGN(std::string folded, mapped(text, UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_COMPAT |
                                                       UTF8PROC_CASEFOLD | UTF8PROC_STRIPMARK));
    // Typographic apostrophes (U+2019, U+2018) and the modifier letter (U+02BC)
    // are all one apostrophe to a reader.
    for (const std::string_view apostrophe : {"\u2019", "\u2018", "\u02BC"}) {
        folded = replace_all(folded, apostrophe, "'");
    }
    return folded;
}

std::vector<CodePoint> code_points(std::string_view text) {
    std::vector<CodePoint> points;
    points.reserve(text.size());
    for (std::size_t at = 0; at < text.size();) {
        utf8proc_int32_t code_point = 0;
        const utf8proc_ssize_t length = next(text, at, code_point);
        if (length <= 0) {
            points.push_back(CodePoint{.value = 0xFFFD, .begin = at, .end = at + 1});
            ++at;
            continue;
        }
        const std::size_t end = at + static_cast<std::size_t>(length);
        points.push_back(CodePoint{.value = static_cast<char32_t>(code_point), .begin = at, .end = end});
        at = end;
    }
    return points;
}

CharClass classify(char32_t code_point) noexcept {
    switch (utf8proc_category(static_cast<utf8proc_int32_t>(code_point))) {
        case UTF8PROC_CATEGORY_LU:
        case UTF8PROC_CATEGORY_LL:
        case UTF8PROC_CATEGORY_LT:
        case UTF8PROC_CATEGORY_LM:
        case UTF8PROC_CATEGORY_LO:
            return CharClass::letter;
        case UTF8PROC_CATEGORY_MN:
        case UTF8PROC_CATEGORY_MC:
        case UTF8PROC_CATEGORY_ME:
            return CharClass::mark;
        case UTF8PROC_CATEGORY_ND:
            return CharClass::digit;
        case UTF8PROC_CATEGORY_PC:
        case UTF8PROC_CATEGORY_PD:
        case UTF8PROC_CATEGORY_PS:
        case UTF8PROC_CATEGORY_PE:
        case UTF8PROC_CATEGORY_PI:
        case UTF8PROC_CATEGORY_PF:
        case UTF8PROC_CATEGORY_PO:
            return CharClass::punctuation;
        case UTF8PROC_CATEGORY_NL:
        case UTF8PROC_CATEGORY_NO:
        case UTF8PROC_CATEGORY_SM:
        case UTF8PROC_CATEGORY_SC:
        case UTF8PROC_CATEGORY_SK:
        case UTF8PROC_CATEGORY_SO:
            return CharClass::symbol;
        case UTF8PROC_CATEGORY_ZS:
        case UTF8PROC_CATEGORY_ZL:
        case UTF8PROC_CATEGORY_ZP:
            return CharClass::space;
        case UTF8PROC_CATEGORY_CC:
            // Tab, line feed, vertical tab, form feed, carriage return.
            return code_point >= 0x09 && code_point <= 0x0D ? CharClass::space : CharClass::other;
        default:
            return CharClass::other;
    }
}

bool is_upper(char32_t code_point) noexcept {
    const utf8proc_category_t category = utf8proc_category(static_cast<utf8proc_int32_t>(code_point));
    return category == UTF8PROC_CATEGORY_LU || category == UTF8PROC_CATEGORY_LT;
}

bool is_lower(char32_t code_point) noexcept {
    return utf8proc_category(static_cast<utf8proc_int32_t>(code_point)) == UTF8PROC_CATEGORY_LL;
}

bool is_opening(char32_t code_point) noexcept {
    const utf8proc_category_t category = utf8proc_category(static_cast<utf8proc_int32_t>(code_point));
    return category == UTF8PROC_CATEGORY_PS || category == UTF8PROC_CATEGORY_PI;
}

bool is_closing(char32_t code_point) noexcept {
    const utf8proc_category_t category = utf8proc_category(static_cast<utf8proc_int32_t>(code_point));
    return category == UTF8PROC_CATEGORY_PE || category == UTF8PROC_CATEGORY_PF;
}

int digit_value(char32_t code_point) noexcept {
    const auto is_digit = [](char32_t candidate) {
        return utf8proc_category(static_cast<utf8proc_int32_t>(candidate)) == UTF8PROC_CATEGORY_ND;
    };
    if (!is_digit(code_point)) {
        return -1;
    }
    // Unicode encodes each script's digits as ten consecutive code points, 0
    // to 9, and keeps it so (its stability policy). Sets may sit next to each
    // other (the mathematical digits are five), so a run of digits is whole
    // sets: the value is the distance from the run's start, modulo ten.
    char32_t first = code_point;
    while (first > 0 && is_digit(first - 1)) {
        --first;
    }
    return static_cast<int>((code_point - first) % 10);
}

}  // namespace rr::text
