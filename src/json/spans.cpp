// Where each value of a JSON text was written (rr/json/spans.hpp).
//
// The text is first parsed by parse(), which refuses whatever is not JSON,
// with its own error. What is left is a reader that may assume it reads
// JSON: it finds where each value begins and ends, and nothing more. It
// keeps its own stack of the objects and arrays it is inside, rather than
// recursing, so that a deeply nested text cannot exhaust the thread's stack.
#include <rr/json/spans.hpp>

#include <map>
#include <string>
#include <utility>

#include <rr/core/assert.hpp>
#include <rr/json/json.hpp>

namespace rr::json {

namespace {

constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";  // parse() skips one at the start

bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool in_number(char c) noexcept {
    return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
}

// Reads a text parse() accepted.
class Scanner {
public:
    explicit Scanner(std::string_view text) : text_(text) {
        if (text_.starts_with(kByteOrderMark)) {
            at_ = kByteOrderMark.size();
        }
    }

    std::vector<Span> scan() && {
        skip_space();
        begin_value("");
        while (!open_.empty()) {
            skip_space();
            const char next = byte(at_);
            if (next == '}' || next == ']') {
                ++at_;
                found_[open_.back().span].end = at_;
                open_.pop_back();
                continue;
            }
            Open& container = open_.back();
            if (container.read > 0) {
                RR_ASSERT(next == ',', "members and elements are separated by commas");
                ++at_;
                skip_space();
            }
            std::string pointer = found_[container.span].pointer + "/";
            if (container.object) {
                const std::size_t key_end = string_end(at_);
                std::string key = key_of(text_.substr(at_, key_end - at_));
                if (const auto earlier = container.members.find(key); earlier != container.members.end()) {
                    drop(earlier->second);  // parse() keeps the later value
                }
                pointer += pointer_token(key);
                container.members.insert_or_assign(std::move(key), found_.size());  // begin_value()'s span
                at_ = key_end;
                skip_space();
                RR_ASSERT(byte(at_) == ':', "a member's key is followed by a colon");
                ++at_;
                skip_space();
            } else {
                pointer += std::to_string(container.read);
            }
            ++container.read;  // before begin_value(), which may open another and move this one
            begin_value(std::move(pointer));
        }
        std::vector<Span> kept;
        kept.reserve(found_.size());
        for (std::size_t index = 0; index < found_.size(); ++index) {
            if (!dropped_[index]) {
                kept.push_back(std::move(found_[index]));
            }
        }
        return kept;
    }

private:
    // An object or array begun and not yet ended.
    struct Open {
        std::size_t span = 0;  // its own, in found_
        bool object = false;
        std::size_t read = 0;                          // its members or elements so far
        std::map<std::string, std::size_t> members{};  // an object's: each key's value, in found_
    };

    [[nodiscard]] char byte(std::size_t index) const {
        RR_ASSERT(index < text_.size(), "the text is JSON: parse() read it to its end");
        return text_[index];
    }

    void skip_space() {
        while (at_ < text_.size() && is_space(text_[at_])) {
            ++at_;
        }
    }

    // The value whose span is found_[index], and every value within it: the
    // spans after it that begin before it ends. It is whole: a later member
    // of its object is being read.
    void drop(std::size_t index) {
        const std::size_t end = found_[index].end;
        for (std::size_t within = index; within < found_.size() && found_[within].begin < end; ++within) {
            dropped_[within] = true;
        }
    }

    // The value beginning at at_, named `pointer`: a scalar, read whole; an
    // object or array, opened, to be ended by scan().
    void begin_value(std::string pointer) {
        const std::size_t begin = at_;
        const char first = byte(begin);
        found_.push_back(Span{.pointer = std::move(pointer), .begin = begin, .end = begin});
        dropped_.push_back(false);
        if (first == '{' || first == '[') {
            open_.push_back(Open{.span = found_.size() - 1, .object = first == '{'});
            ++at_;
            return;
        }
        at_ = scalar_end(begin);
        found_.back().end = at_;
    }

    [[nodiscard]] std::size_t scalar_end(std::size_t begin) const {
        switch (byte(begin)) {
            case '"':
                return string_end(begin);
            case 't':  // true
            case 'n':  // null
                return begin + 4;
            case 'f':  // false
                return begin + 5;
            default: {  // a number
                std::size_t end = begin;
                while (end < text_.size() && in_number(text_[end])) {
                    ++end;
                }
                return end;
            }
        }
    }

    // One past the quote that closes the string opened at `begin`: a
    // backslash escapes the byte after it, '"' and '\' among them.
    [[nodiscard]] std::size_t string_end(std::size_t begin) const {
        RR_ASSERT(byte(begin) == '"', "a string, and a key, begin with a quote");
        std::size_t at = begin + 1;
        while (byte(at) != '"') {
            at += byte(at) == '\\' ? std::size_t{2} : std::size_t{1};
        }
        return at + 1;
    }

    // A key as written, quotes and escapes included, as the text it stands for.
    [[nodiscard]] static std::string key_of(std::string_view quoted) {
        Result<Json> key = parse(quoted);
        RR_ASSERT(key.has_value() && key->is_string(), "a key parse() read is a string");
        return key->get<std::string>();
    }

    std::string_view text_;
    std::size_t at_ = 0;
    std::vector<Span> found_;
    std::vector<bool> dropped_;  // by found_'s index: a value parse() does not keep
    std::vector<Open> open_;
};

}  // namespace

std::string pointer_token(std::string_view key) {
    std::string token;
    token.reserve(key.size());
    for (const char c : key) {
        if (c == '~') {
            token += "~0";
        } else if (c == '/') {
            token += "~1";
        } else {
            token += c;
        }
    }
    return token;
}

Result<std::vector<Span>> spans(std::string_view text) {
    RR_TRY(parse(text));
    return Scanner{text}.scan();
}

}  // namespace rr::json
