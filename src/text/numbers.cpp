#include <rr/text/numbers.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <rr/text/tokens.hpp>
#include <rr/text/unicode.hpp>

namespace rr::text {

namespace {

// What a number word does in a number.
enum class Kind : std::uint8_t {
    unit,     // 0–9
    teen,     // 10–19
    tens,     // 20, 30, … 90
    hundred,  // multiplies the group before it
    dozen,    // multiplies it by twelve
    scale,    // thousand, million, …: closes a group
};

struct NumberWord {
    std::string_view word;
    Kind kind = Kind::unit;
    double value = 0.0;
    bool ordinal = false;
};

// Sorted by word, byte order: looked up by binary search.
constexpr auto kNumberWords = std::to_array<NumberWord>({
    {.word = "billion", .kind = Kind::scale, .value = 1e9, .ordinal = false},
    {.word = "billionth", .kind = Kind::scale, .value = 1e9, .ordinal = true},
    {.word = "dozen", .kind = Kind::dozen, .value = 12, .ordinal = false},
    {.word = "eight", .kind = Kind::unit, .value = 8, .ordinal = false},
    {.word = "eighteen", .kind = Kind::teen, .value = 18, .ordinal = false},
    {.word = "eighteenth", .kind = Kind::teen, .value = 18, .ordinal = true},
    {.word = "eighth", .kind = Kind::unit, .value = 8, .ordinal = true},
    {.word = "eightieth", .kind = Kind::tens, .value = 80, .ordinal = true},
    {.word = "eighty", .kind = Kind::tens, .value = 80, .ordinal = false},
    {.word = "eleven", .kind = Kind::teen, .value = 11, .ordinal = false},
    {.word = "eleventh", .kind = Kind::teen, .value = 11, .ordinal = true},
    {.word = "fifteen", .kind = Kind::teen, .value = 15, .ordinal = false},
    {.word = "fifteenth", .kind = Kind::teen, .value = 15, .ordinal = true},
    {.word = "fifth", .kind = Kind::unit, .value = 5, .ordinal = true},
    {.word = "fiftieth", .kind = Kind::tens, .value = 50, .ordinal = true},
    {.word = "fifty", .kind = Kind::tens, .value = 50, .ordinal = false},
    {.word = "first", .kind = Kind::unit, .value = 1, .ordinal = true},
    {.word = "five", .kind = Kind::unit, .value = 5, .ordinal = false},
    {.word = "fortieth", .kind = Kind::tens, .value = 40, .ordinal = true},
    {.word = "forty", .kind = Kind::tens, .value = 40, .ordinal = false},
    {.word = "four", .kind = Kind::unit, .value = 4, .ordinal = false},
    {.word = "fourteen", .kind = Kind::teen, .value = 14, .ordinal = false},
    {.word = "fourteenth", .kind = Kind::teen, .value = 14, .ordinal = true},
    {.word = "fourth", .kind = Kind::unit, .value = 4, .ordinal = true},
    {.word = "hundred", .kind = Kind::hundred, .value = 100, .ordinal = false},
    {.word = "hundredth", .kind = Kind::hundred, .value = 100, .ordinal = true},
    {.word = "million", .kind = Kind::scale, .value = 1e6, .ordinal = false},
    {.word = "millionth", .kind = Kind::scale, .value = 1e6, .ordinal = true},
    {.word = "nine", .kind = Kind::unit, .value = 9, .ordinal = false},
    {.word = "nineteen", .kind = Kind::teen, .value = 19, .ordinal = false},
    {.word = "nineteenth", .kind = Kind::teen, .value = 19, .ordinal = true},
    {.word = "ninetieth", .kind = Kind::tens, .value = 90, .ordinal = true},
    {.word = "ninety", .kind = Kind::tens, .value = 90, .ordinal = false},
    {.word = "ninth", .kind = Kind::unit, .value = 9, .ordinal = true},
    {.word = "nought", .kind = Kind::unit, .value = 0, .ordinal = false},
    {.word = "one", .kind = Kind::unit, .value = 1, .ordinal = false},
    {.word = "second", .kind = Kind::unit, .value = 2, .ordinal = true},
    {.word = "seven", .kind = Kind::unit, .value = 7, .ordinal = false},
    {.word = "seventeen", .kind = Kind::teen, .value = 17, .ordinal = false},
    {.word = "seventeenth", .kind = Kind::teen, .value = 17, .ordinal = true},
    {.word = "seventh", .kind = Kind::unit, .value = 7, .ordinal = true},
    {.word = "seventieth", .kind = Kind::tens, .value = 70, .ordinal = true},
    {.word = "seventy", .kind = Kind::tens, .value = 70, .ordinal = false},
    {.word = "six", .kind = Kind::unit, .value = 6, .ordinal = false},
    {.word = "sixteen", .kind = Kind::teen, .value = 16, .ordinal = false},
    {.word = "sixteenth", .kind = Kind::teen, .value = 16, .ordinal = true},
    {.word = "sixth", .kind = Kind::unit, .value = 6, .ordinal = true},
    {.word = "sixtieth", .kind = Kind::tens, .value = 60, .ordinal = true},
    {.word = "sixty", .kind = Kind::tens, .value = 60, .ordinal = false},
    {.word = "ten", .kind = Kind::teen, .value = 10, .ordinal = false},
    {.word = "tenth", .kind = Kind::teen, .value = 10, .ordinal = true},
    {.word = "third", .kind = Kind::unit, .value = 3, .ordinal = true},
    {.word = "thirteen", .kind = Kind::teen, .value = 13, .ordinal = false},
    {.word = "thirteenth", .kind = Kind::teen, .value = 13, .ordinal = true},
    {.word = "thirtieth", .kind = Kind::tens, .value = 30, .ordinal = true},
    {.word = "thirty", .kind = Kind::tens, .value = 30, .ordinal = false},
    {.word = "thousand", .kind = Kind::scale, .value = 1e3, .ordinal = false},
    {.word = "thousandth", .kind = Kind::scale, .value = 1e3, .ordinal = true},
    {.word = "three", .kind = Kind::unit, .value = 3, .ordinal = false},
    {.word = "trillion", .kind = Kind::scale, .value = 1e12, .ordinal = false},
    {.word = "trillionth", .kind = Kind::scale, .value = 1e12, .ordinal = true},
    {.word = "twelfth", .kind = Kind::teen, .value = 12, .ordinal = true},
    {.word = "twelve", .kind = Kind::teen, .value = 12, .ordinal = false},
    {.word = "twentieth", .kind = Kind::tens, .value = 20, .ordinal = true},
    {.word = "twenty", .kind = Kind::tens, .value = 20, .ordinal = false},
    {.word = "two", .kind = Kind::unit, .value = 2, .ordinal = false},
    {.word = "zero", .kind = Kind::unit, .value = 0, .ordinal = false},
    {.word = "zeroth", .kind = Kind::unit, .value = 0, .ordinal = true},
});
static_assert(std::ranges::is_sorted(kNumberWords, std::less{}, &NumberWord::word));

// Words that make the number before them a fraction ("two thirds"), besides
// the singular ordinals from "third" up ("one third"). Sorted.
constexpr auto kFractionWords = std::to_array<std::string_view>({
    "billionths",  "eighteenths", "eighths",     "eightieths",  "elevenths", "fifteenths", "fifths",
    "fiftieths",   "fortieths",   "fourteenths", "fourths",     "half",      "halves",     "hundredths",
    "millionths",  "nineteenths", "ninetieths",  "ninths",      "quarter",   "quarters",   "seventeenths",
    "sevenths",    "seventieths", "sixteenths",  "sixths",      "sixtieths", "tenths",     "thirds",
    "thirteenths", "thirtieths",  "thousandths", "trillionths", "twelfths",  "twentieths",
});
static_assert(std::ranges::is_sorted(kFractionWords));

// A token, as the reader sees it.
struct Lexeme {
    TokenKind kind = TokenKind::word;
    Span span{};
    std::string folded{};  // fold()ed; the token's own text if it cannot be
    bool joined = false;   // it touches the token before it: no space between
};

// A quantity read from the lexemes up to `end`.
struct Reading {
    double value = 0.0;
    std::size_t end = 0;
    double scale = 1.0;  // the scale word it ends with ("10 million": 1e6), else 1
    int scales = 0;      // how many scale words it has
    bool ordinal = false;
    bool closed = false;  // nothing may follow: an ordinal, a year, a decade
    bool words = false;   // written with number words
    bool digits = false;  // written with digits
};

// A mention while the text is being read: lexemes [first, end).
struct Found {
    std::size_t first = 0;
    std::size_t end = 0;
    Reading reading{};
    bool negative = false;
    bool percent = false;
    bool fraction = false;  // a fraction, passed over: not a number
};

// How a reading is written: in digits, words, or both ("1.5 million").
NumberForm form_of(const Reading& reading) noexcept {
    if (reading.words && reading.digits) {
        return NumberForm::mixed;
    }
    return reading.words ? NumberForm::words : NumberForm::digits;
}

// The value of digits of any script, with thousands commas and a decimal
// point ("1,234.5"); none when it is out of range.
std::optional<double> digits_value(std::string_view digits) {
    std::string ascii = "0";  // ".5" reads as "0.5"
    for (const CodePoint& point : code_points(digits)) {
        if (const int digit = digit_value(point.value); digit >= 0) {
            ascii.push_back(static_cast<char>('0' + digit));
        } else if (point.value == U'.') {
            ascii.push_back('.');
        }
    }
    double value = 0.0;
    const auto [end, error] = std::from_chars(ascii.data(), ascii.data() + ascii.size(), value);
    if (error != std::errc{} || end != ascii.data() + ascii.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

class Reader {
public:
    explicit Reader(std::string_view text) : text_(text) {
        for (const Token& token : tokenize(text)) {
            const std::string_view raw = text_of(text, token.span);
            Result<std::string> folded = fold(raw);
            const bool joined = !lexemes_.empty() && lexemes_.back().span.end == token.span.begin;
            lexemes_.push_back(Lexeme{.kind = token.kind,
                                      .span = token.span,
                                      .folded = folded.has_value() ? std::move(*folded) : std::string{raw},
                                      .joined = joined});
        }
    }

    [[nodiscard]] std::vector<NumberMention> read() const {
        std::vector<Found> found;
        for (std::size_t index = 0; index < lexemes_.size();) {
            if (const std::optional<Found> mention = mention_at(index)) {
                if (!mention->fraction) {
                    found.push_back(*mention);
                }
                index = mention->end;
            } else {
                ++index;
            }
        }
        share_range_units(found);

        std::vector<NumberMention> mentions;
        mentions.reserve(found.size());
        for (const Found& mention : found) {
            const Reading& reading = mention.reading;
            mentions.push_back(NumberMention{.value = mention.negative ? -reading.value : reading.value,
                                             .percent = mention.percent,
                                             .ordinal = reading.ordinal,
                                             .form = form_of(reading),
                                             .span = {.begin = lexemes_[mention.first].span.begin,
                                                      .end = lexemes_[mention.end - 1].span.end}});
        }
        return mentions;
    }

private:
    [[nodiscard]] const Lexeme* lexeme(std::size_t index) const {
        return index < lexemes_.size() ? &lexemes_[index] : nullptr;
    }

    [[nodiscard]] bool is(std::size_t index, std::string_view word) const {
        const Lexeme* found = lexeme(index);
        return found != nullptr && found->folded == word;
    }

    [[nodiscard]] std::optional<NumberWord> number_word(std::size_t index) const {
        const Lexeme* found = lexeme(index);
        if (found == nullptr || found->kind != TokenKind::word) {
            return std::nullopt;
        }
        const auto* const entry =
            std::ranges::lower_bound(kNumberWords, found->folded, std::less{}, &NumberWord::word);
        if (entry == kNumberWords.end() || entry->word != found->folded) {
            return std::nullopt;
        }
        return *entry;
    }

    // Past a hyphen that joins two words ("thirty-seven"), or `index` itself.
    [[nodiscard]] std::size_t past_hyphen(std::size_t index) const {
        const Lexeme* hyphen = lexeme(index);
        const Lexeme* after = lexeme(index + 1);
        const bool joins = hyphen != nullptr && (hyphen->folded == "-" || hyphen->folded == "\u2010") &&
                           hyphen->joined && after != nullptr && after->joined;
        return joins ? index + 1 : index;
    }

    // A number, its sign before it and its percent after it.
    [[nodiscard]] std::optional<Found> mention_at(std::size_t first) const {
        std::size_t index = first;
        bool negative = false;
        if (is(index, "minus") || is(index, "negative") || is_sign(index)) {
            negative = true;
            ++index;
        }
        std::optional<Reading> reading = year(index);
        if (!reading.has_value()) {
            reading = magnitude(index);
        }
        if (!reading.has_value() || !std::isfinite(reading->value) || is_not_a_number(index, *reading)) {
            return std::nullopt;
        }
        if (const std::optional<std::size_t> end = fraction_end(*reading)) {
            return Found{.first = first, .end = *end, .fraction = true};
        }
        Found found{.first = first, .end = reading->end, .reading = *reading, .negative = negative};
        if (!reading->ordinal) {
            if (is(found.end, "%") || is(found.end, "\u066A") || is(found.end, "percent")) {
                found.percent = true;
                found.end += 1;
            } else if (is(found.end, "per") && is(found.end + 1, "cent")) {
                found.percent = true;
                found.end += 2;
            }
        }
        return found;
    }

    // Whether the lexeme at `index` opens a phrase: "(", "[", a quote.
    [[nodiscard]] bool opens(std::size_t index) const {
        const Lexeme* found = lexeme(index);
        if (found == nullptr || found->kind != TokenKind::punctuation) {
            return false;
        }
        const char32_t code_point = code_points(text_of(text_, found->span)).front().value;
        return is_opening(code_point) || code_point == U'"' || code_point == U'\'';
    }

    // A minus sign before a number it touches, after a space or an opening
    // bracket: "-5", "(-5", but not "COVID-19", "10-20" or "5%-10%".
    [[nodiscard]] bool is_sign(std::size_t index) const {
        const Lexeme* sign = lexeme(index);
        const Lexeme* number = lexeme(index + 1);
        return sign != nullptr && (sign->folded == "-" || sign->folded == "\u2212") && number != nullptr &&
               number->joined && (number->kind == TokenKind::number || number->folded == ".") &&
               (!sign->joined || opens(index - 1));
    }

    // "One" as a pronoun ("no one", "one another"), and "second" as time ("a
    // second"): a single word, judged by its neighbours.
    [[nodiscard]] bool is_not_a_number(std::size_t index, const Reading& reading) const {
        if (reading.end != index + 1) {
            return false;
        }
        const std::string_view before = index > 0 ? std::string_view{lexemes_[index - 1].folded} : "";
        if (is(index, "one")) {
            static constexpr auto kPronounDeterminers = std::to_array<std::string_view>(
                {"any", "each", "every", "no", "that", "the", "this", "which"});
            return std::ranges::find(kPronounDeterminers, before) != kPronounDeterminers.end() ||
                   is(index + 1, "another");
        }
        if (is(index, "second")) {
            static constexpr auto kTimeDeterminers = std::to_array<std::string_view>(
                {"a", "an", "each", "every", "one", "per", "split", "that", "this"});
            return std::ranges::find(kTimeDeterminers, before) != kTimeDeterminers.end();
        }
        // "a third of": a fraction, where "a third time" is an ordinal.
        const std::optional<NumberWord> word = number_word(index);
        return word.has_value() && is_fraction_ordinal(*word) && (before == "a" || before == "an") &&
               is(index + 1, "of");
    }

    // Where a fraction ends, if the quantity read starts one: "one third",
    // "two thirds", "three quarters", "one hundredth of", "a thousandth of".
    [[nodiscard]] std::optional<std::size_t> fraction_end(const Reading& reading) const {
        const std::optional<NumberWord> last = number_word(reading.end - 1);
        const bool ordinal_multiplier =
            reading.ordinal && last.has_value() && (last->kind == Kind::hundred || last->kind == Kind::scale);
        if (ordinal_multiplier && is(reading.end, "of")) {
            return reading.end;
        }
        if (reading.closed) {
            return std::nullopt;
        }
        const Lexeme* next = lexeme(reading.end);
        if (next != nullptr && std::ranges::binary_search(kFractionWords, next->folded)) {
            return reading.end + 1;
        }
        if (const std::optional<NumberWord> word = number_word(reading.end);
            word.has_value() && is_fraction_ordinal(*word)) {
            return reading.end + 1;
        }
        return std::nullopt;
    }

    // An ordinal that also names a fraction: "third" and up, not "first" or
    // "second" ("one second" is time).
    static bool is_fraction_ordinal(const NumberWord& word) {
        return word.ordinal && word.value >= 3 && word.kind != Kind::hundred && word.kind != Kind::scale;
    }

    // A year said in two pairs of digits: "nineteen eighty-four", "twenty
    // twenty", "nineteen oh five". The first pair is 15 to 20.
    [[nodiscard]] std::optional<Reading> year(std::size_t index) const {
        const std::optional<NumberWord> century = number_word(index);
        if (!century.has_value() || century->ordinal || century->value < 15 || century->value > 20) {
            return std::nullopt;
        }
        if (const std::optional<Reading> rest = small(index + 1); rest.has_value() && rest->value >= 10) {
            return Reading{.value = (century->value * 100) + rest->value,
                           .end = rest->end,
                           .ordinal = rest->ordinal,
                           .closed = true,
                           .words = true};
        }
        if (is(index + 1, "oh") || is(index + 1, "o")) {
            const std::optional<NumberWord> unit = number_word(index + 2);
            if (unit.has_value() && unit->kind == Kind::unit && unit->value >= 1) {
                return Reading{.value = (century->value * 100) + unit->value,
                               .end = index + 3,
                               .ordinal = unit->ordinal,
                               .closed = true,
                               .words = true};
            }
        }
        return std::nullopt;
    }

    // magnitude := hundreds { scale [ "and" ] [ hundreds ] }, scales strictly
    // decreasing; "and a half" may follow a scale ("a million and a half").
    [[nodiscard]] std::optional<Reading> magnitude(std::size_t index) const {
        const std::optional<Reading> first = hundreds(index);
        if (!first.has_value() || first->closed) {
            return first;
        }
        Reading reading = *first;
        double total = 0.0;
        double group = first->value;
        double last_scale = std::numeric_limits<double>::infinity();
        while (true) {
            const std::size_t at = past_hyphen(reading.end);
            const std::optional<NumberWord> scale = number_word(at);
            // Smaller than the last scale: the group before it was read only
            // because the scale after it may follow (below).
            if (!scale.has_value() || scale->kind != Kind::scale) {
                break;
            }
            total += group * scale->value;
            group = 0.0;
            last_scale = scale->value;
            reading.end = at + 1;
            reading.scale = scale->value;
            reading.scales += 1;
            reading.words = true;
            reading.ordinal = scale->ordinal;
            reading.closed = scale->ordinal;
            if (reading.closed) {
                break;
            }
            if (is_and_a_half(reading.end)) {
                total += 0.5 * scale->value;
                reading.end += 3;
                break;
            }
            const std::size_t next = is(reading.end, "and") ? reading.end + 1 : reading.end;
            const std::optional<Reading> more = hundreds(next);
            if (!more.has_value()) {
                break;
            }
            // "five thousand three million" is two numbers: the group belongs
            // to the scale after it, which cannot follow this one.
            const std::optional<NumberWord> after = number_word(past_hyphen(more->end));
            if (after.has_value() && after->kind == Kind::scale && after->value >= last_scale) {
                break;
            }
            group = more->value;
            reading.end = more->end;
            reading.scale = 1.0;
            reading.ordinal = more->ordinal;
            reading.closed = more->closed;
            reading.digits = reading.digits || more->digits;
            if (reading.closed) {
                break;
            }
        }
        reading.value = total + group;
        return reading;
    }

    // hundreds := lead [ "hundred" [ "and" ] [ small ] | "dozen" ] [ fraction ]
    [[nodiscard]] std::optional<Reading> hundreds(std::size_t index) const {
        std::optional<Reading> reading = lead(index);
        if (!reading.has_value() || reading->closed) {
            return reading;
        }
        const std::size_t at = past_hyphen(reading->end);
        const std::optional<NumberWord> word = number_word(at);
        const bool whole = reading->value == std::floor(reading->value) && reading->value >= 1;
        if (word.has_value() && word->kind == Kind::hundred && whole && reading->value < 100) {
            multiply(*reading, *word, at);
            if (!reading->closed) {
                const std::size_t rest_at = is(reading->end, "and") ? reading->end + 1 : reading->end;
                const std::optional<Reading> rest = small(rest_at);
                // "one hundred and two hundred" is two numbers.
                const std::optional<NumberWord> after =
                    rest.has_value() ? number_word(past_hyphen(rest->end)) : std::optional<NumberWord>{};
                if (rest.has_value() && rest->value >= 1 &&
                    (!after.has_value() || after->kind != Kind::hundred)) {
                    reading->value += rest->value;
                    reading->end = rest->end;
                    reading->ordinal = rest->ordinal;
                    reading->closed = rest->closed;
                }
            }
        } else if (word.has_value() && word->kind == Kind::dozen && whole) {
            multiply(*reading, *word, at);
        }
        if (!reading->closed) {
            add_fraction(*reading);
        }
        return reading;
    }

    static void multiply(Reading& reading, const NumberWord& word, std::size_t at) {
        reading.value *= word.value;
        reading.end = at + 1;
        reading.words = true;
        reading.ordinal = word.ordinal;
        reading.closed = word.ordinal;
    }

    // lead := small | digits | "a" | "an" (only before a multiplier: "a hundred")
    [[nodiscard]] std::optional<Reading> lead(std::size_t index) const {
        if (std::optional<Reading> words = small(index)) {
            return words;
        }
        if (std::optional<Reading> written = digits(index)) {
            return written;
        }
        // An ordinal multiplier stands alone: "the hundredth time".
        if (const std::optional<NumberWord> word = number_word(index);
            word.has_value() && word->ordinal && (word->kind == Kind::hundred || word->kind == Kind::scale)) {
            return Reading{
                .value = word->value, .end = index + 1, .ordinal = true, .closed = true, .words = true};
        }
        if (is(index, "a") || is(index, "an")) {
            const std::optional<NumberWord> next = number_word(index + 1);
            if (next.has_value() &&
                (next->kind == Kind::hundred || next->kind == Kind::dozen || next->kind == Kind::scale)) {
                return Reading{.value = 1.0, .end = index + 1, .words = true};
            }
        }
        return std::nullopt;
    }

    // small := unit | teen | tens [ "-" ] [ unit ]: 0 to 99.
    [[nodiscard]] std::optional<Reading> small(std::size_t index) const {
        const std::optional<NumberWord> word = number_word(index);
        if (!word.has_value() ||
            (word->kind != Kind::unit && word->kind != Kind::teen && word->kind != Kind::tens)) {
            return std::nullopt;
        }
        Reading reading{.value = word->value,
                        .end = index + 1,
                        .ordinal = word->ordinal,
                        .closed = word->ordinal,
                        .words = true};
        if (word->kind == Kind::tens && !word->ordinal) {
            const std::size_t at = past_hyphen(index + 1);
            const std::optional<NumberWord> unit = number_word(at);
            if (unit.has_value() && unit->kind == Kind::unit && unit->value >= 1) {
                reading.value += unit->value;
                reading.end = at + 1;
                reading.ordinal = unit->ordinal;
                reading.closed = unit->ordinal;
            }
        }
        return reading;
    }

    // A number written in digits: "37", "1,234.5", ".5", "21st", "1990s".
    [[nodiscard]] std::optional<Reading> digits(std::size_t index) const {
        const Lexeme* found = lexeme(index);
        if (found == nullptr) {
            return std::nullopt;
        }
        const std::string_view raw = text_of(text_, found->span);
        if (found->kind == TokenKind::number) {
            const std::optional<double> value = digits_value(raw);
            if (!value.has_value()) {
                return std::nullopt;
            }
            return Reading{.value = *value, .end = index + 1, .digits = true};
        }
        // ".5": a point that starts a number, before digits it touches, and
        // after no word or number it touches ("-.5", "$.50"; not "end.5").
        const Lexeme* next = lexeme(index + 1);
        const bool starts = !found->joined || (lexemes_[index - 1].kind != TokenKind::word &&
                                               lexemes_[index - 1].kind != TokenKind::number);
        if (found->folded == "." && starts && next != nullptr && next->kind == TokenKind::number &&
            next->joined && next->folded.find_first_of(".,") == std::string::npos) {
            const std::optional<double> value = digits_value(std::string{"."} + next->folded);
            if (!value.has_value()) {
                return std::nullopt;
            }
            return Reading{.value = *value, .end = index + 2, .digits = true};
        }
        if (found->kind == TokenKind::word) {
            return digits_with_suffix(index, *found);
        }
        return std::nullopt;
    }

    // "21st", "2nd", "3rd", "4th" (ordinals) and "1990s" (a decade).
    [[nodiscard]] static std::optional<Reading> digits_with_suffix(std::size_t index, const Lexeme& word) {
        const std::string_view folded = word.folded;
        const std::size_t suffix = folded.find_first_not_of("0123456789,.");
        if (suffix == 0 || suffix == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view tail = folded.substr(suffix);
        const bool ordinal = tail == "st" || tail == "nd" || tail == "rd" || tail == "th";
        if (!ordinal && tail != "s") {
            return std::nullopt;
        }
        const std::optional<double> value = digits_value(folded.substr(0, suffix));
        if (!value.has_value()) {
            return std::nullopt;
        }
        return Reading{.value = *value, .end = index + 1, .ordinal = ordinal, .closed = true, .digits = true};
    }

    [[nodiscard]] bool is_and_a_half(std::size_t index) const {
        return is(index, "and") && is(index + 1, "a") && is(index + 2, "half");
    }

    // "and a half", or "point" and digits in words: "two and a half", "three
    // point one four".
    void add_fraction(Reading& reading) const {
        if (is_and_a_half(reading.end)) {
            reading.value += 0.5;
            reading.end += 3;
            reading.words = true;
            return;
        }
        constexpr double kExactIntegers = 9007199254740992.0;  // 2^53
        if (!is(reading.end, "point") || reading.value != std::floor(reading.value) ||
            reading.value >= kExactIntegers) {
            return;
        }
        std::string decimals;
        std::size_t index = reading.end + 1;
        while (true) {
            if (is(index, "oh") || is(index, "o")) {
                decimals.push_back('0');
            } else if (const std::optional<NumberWord> digit = number_word(index);
                       digit.has_value() && digit->kind == Kind::unit && !digit->ordinal) {
                decimals.push_back(static_cast<char>('0' + static_cast<int>(digit->value)));
            } else {
                break;
            }
            ++index;
        }
        if (decimals.empty()) {
            return;
        }
        // Read as the digits would be ("3.14"), so both forms give one double.
        const std::optional<double> value =
            digits_value(std::format("{}.{}", static_cast<long long>(reading.value), decimals));
        if (value.has_value()) {
            reading.value = *value;
            reading.end = index;
            reading.words = true;
        }
    }

    // In "5-10%", "5 to 10 percent" and "between 5 and 10 million", the first
    // end takes the second's unit (see the header comment).
    void share_range_units(std::vector<Found>& found) const {
        for (std::size_t index = 0; index + 1 < found.size(); ++index) {
            Found& low = found[index];
            const Found& high = found[index + 1];
            const Reading& plain = low.reading;
            if (high.first != low.end + 1 || !is_range_word(low.end) || low.percent || plain.closed ||
                plain.scales > 0) {
                continue;
            }
            if (high.percent) {
                low.percent = true;
            }
            const Reading& upper = high.reading;
            if (upper.scales == 1 && upper.scale > 1.0 && !high.percent &&
                plain.value <= 10.0 * (upper.value / upper.scale)) {
                low.reading.value *= upper.scale;
            }
        }
    }

    [[nodiscard]] bool is_range_word(std::size_t index) const {
        return is(index, "-") || is(index, "\u2013") || is(index, "to") || is(index, "and") ||
               is(index, "or");
    }

    std::string_view text_;
    std::vector<Lexeme> lexemes_;
};

}  // namespace

std::vector<NumberMention> numbers(std::string_view text) {
    return Reader{text}.read();
}

bool same_value(double lhs, double rhs) noexcept {
    constexpr double kRelativeTolerance = 1e-9;
    return lhs == rhs || std::abs(lhs - rhs) <= kRelativeTolerance * std::max(std::abs(lhs), std::abs(rhs));
}

bool same_number(const NumberMention& lhs, const NumberMention& rhs) noexcept {
    return same_value(lhs.value, rhs.value) && lhs.percent == rhs.percent && lhs.ordinal == rhs.ordinal;
}

}  // namespace rr::text
