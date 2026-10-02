// toml_io.hpp — the eval's files, read strictly and written plainly (private to
// rr_eval).
//
// The eval's files are written by people: a set of videos, a gold
// annotation corrected by hand. A misspelt key that was silently ignored
// would leave a video without its split, or a fact without its importance,
// and the eval would measure something else than its author meant. So every
// table is read through a Table: each key is asked for by name, with its
// type, and a key nobody asked for is an error that names it and where it
// is. rr_reason reads its tiers the same way, with defaults where these
// files require values; a shared reader waits for a third format.
#pragma once

#include <cstdint>
#include <format>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <toml++/toml.hpp>

#include <rr/core/result.hpp>
#include <rr/core/strings.hpp>
#include <rr/core/time_span.hpp>

namespace rr::eval::detail {

class Table {
public:
    // `where` names the table in messages: "eval/sets/m2.toml, video 3".
    Table(const toml::table& table, std::string where) : table_(&table), where_(std::move(where)) {}

    // A text that must be there, and not be empty.
    [[nodiscard]] Result<std::string> text(std::string_view key) {
        RR_TRY_ASSIGN(std::optional<std::string> value, optional_text(key));
        if (!value.has_value()) {  // optional_text() takes an empty text for none
            return wrong(key, "a text, and it is missing or empty");
        }
        return *std::move(value);
    }

    // A text that may be left out; an empty one is the same as none.
    [[nodiscard]] Result<std::optional<std::string>> optional_text(std::string_view key) {
        const toml::node* value = node(key);
        if (value == nullptr) {
            return std::optional<std::string>{};
        }
        const auto* text = value->as_string();
        if (text == nullptr) {
            return wrong(key, "a text in quotes");
        }
        if (text->get().empty()) {
            return std::optional<std::string>{};
        }
        return std::optional<std::string>{text->get()};
    }

    // One of `allowed`, which must be there.
    [[nodiscard]] Result<std::string> choice(std::string_view key,
                                             std::span<const std::string_view> allowed) {
        RR_TRY_ASSIGN(std::string value, text(key));
        if (std::ranges::find(allowed, std::string_view{value}) == allowed.end()) {
            return wrong(key, std::format("one of: {}", join(allowed, ", ")));
        }
        return value;
    }

    // A whole number in [least, most], which must be there.
    [[nodiscard]] Result<std::int64_t> integer(std::string_view key, std::int64_t least, std::int64_t most) {
        const toml::node* value = node(key);
        const auto* number = value == nullptr ? nullptr : value->as_integer();
        if (number == nullptr || number->get() < least || number->get() > most) {
            return wrong(key, std::format("a whole number from {} to {}", least, most));
        }
        return number->get();
    }

    // A whole number in [least, most] that may be left out.
    [[nodiscard]] Result<std::optional<std::int64_t>> optional_integer(std::string_view key,
                                                                       std::int64_t least,
                                                                       std::int64_t most) {
        if (table_->get(key) == nullptr) {
            return std::optional<std::int64_t>{};
        }
        RR_TRY_ASSIGN(const std::int64_t value, integer(key, least, most));
        return std::optional<std::int64_t>{value};
    }

    // A true or false, which must be there.
    [[nodiscard]] Result<bool> boolean(std::string_view key) {
        const toml::node* value = node(key);
        const auto* flag = value == nullptr ? nullptr : value->as_boolean();
        if (flag == nullptr) {
            return wrong(key, "true or false");
        }
        return flag->get();
    }

    // Marks `key` as read elsewhere (an array of tables, read table by table).
    void allow(std::string_view key) { read_.emplace(key); }

    // A time in the video, as players show it: "3:04", "1:02:03", "3:04.250".
    [[nodiscard]] Result<Millis> time(std::string_view key) {
        RR_TRY_ASSIGN(const std::string value, text(key));
        Result<Millis> parsed = parse_timestamp(value);
        if (!parsed.has_value()) {
            return wrong(key, std::format("a time such as \"3:04\" or \"1:02:03.250\", not \"{}\"", value));
        }
        return *parsed;
    }

    // Errc::invalid_argument for a key nobody asked for.
    [[nodiscard]] Result<void> nothing_else() const {
        for (const auto& [key, value] : *table_) {
            if (!read_.contains(key.str())) {
                return fail(Errc::invalid_argument, std::format("{}: unknown key '{}'", where_, key.str()));
            }
        }
        return ok();
    }

    [[nodiscard]] const std::string& where() const noexcept { return where_; }

private:
    [[nodiscard]] const toml::node* node(std::string_view key) {
        read_.emplace(key);
        return table_->get(key);
    }

    [[nodiscard]] std::unexpected<Error> wrong(std::string_view key, std::string_view expected) const {
        return std::unexpected(
            Error{Errc::invalid_argument, std::format("{}: '{}' must be {}", where_, key, expected)});
    }

    const toml::table* table_;
    std::string where_;
    std::set<std::string, std::less<>> read_;
};

// The tables of the array `key` of `document` ([[key]] entries), each with
// where it is ("<file>, <key> 3"). None when the document has no such array.
// Errc::invalid_argument when `key` is something else.
template <typename Visit>
[[nodiscard]] Result<void> each_table(const toml::table& document, std::string_view key,
                                      std::string_view file, Visit&& visit) {
    const toml::node* found = document.get(key);
    if (found == nullptr) {
        return ok();
    }
    const auto* array = found->as_array();
    if (array == nullptr || !array->is_array_of_tables()) {
        return fail(Errc::invalid_argument, std::format("{}: '{}' must be [[{}]] tables", file, key, key));
    }
    std::size_t number = 0;
    for (const toml::node& entry : *array) {
        ++number;
        Table table{*entry.as_table(), std::format("{}, {} {}", file, key, number)};
        RR_TRY(visit(table));
        RR_TRY(table.nothing_else());
    }
    return ok();
}

// `text` as a TOML basic string: quotes, backslashes and control characters
// escaped, everything else as it is (TOML is UTF-8).
[[nodiscard]] inline std::string toml_string(std::string_view text) {
    std::string written = "\"";
    for (const char character : text) {
        switch (character) {
            case '"':
                written += "\\\"";
                break;
            case '\\':
                written += "\\\\";
                break;
            case '\n':
                written += "\\n";
                break;
            case '\t':
                written += "\\t";
                break;
            case '\r':
                written += "\\r";
                break;
            default:
                if (const auto byte = static_cast<unsigned char>(character); byte < 0x20U || byte == 0x7FU) {
                    written += std::format("\\u{:04X}", byte);
                } else {
                    written += character;
                }
        }
    }
    return written + '"';
}

// `text` parsed as TOML, or Errc::invalid_argument naming the line.
[[nodiscard]] inline Result<toml::table> parse_toml(std::string_view text, std::string_view file) {
    try {
        return toml::parse(text, std::string_view{file});
    } catch (const toml::parse_error& error) {
        return fail(Errc::invalid_argument,
                    std::format("{}, line {}: {}", file, error.source().begin.line, error.description()));
    }
}

}  // namespace rr::eval::detail
