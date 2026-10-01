#include <rr/text/unicode.hpp>

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <rr/core/error.hpp>
#include <rr/testkit/result.hpp>

namespace rr::text {
namespace {

using testkit::unwrap;

TEST(Unicode, ValidUtf8IsTold) {
    EXPECT_TRUE(valid_utf8(""));
    EXPECT_TRUE(valid_utf8("plain ASCII"));
    EXPECT_TRUE(valid_utf8("caf\u00E9 \u20AC \U0001F600"));
    EXPECT_FALSE(valid_utf8("\x80")) << "a lone continuation byte";
    EXPECT_FALSE(valid_utf8("\xE2\x82")) << "a truncated sequence";
    EXPECT_FALSE(valid_utf8("\xC0\xAF")) << "an overlong encoding";
    EXPECT_FALSE(valid_utf8("\xED\xA0\x80")) << "a surrogate";
    EXPECT_FALSE(valid_utf8("\xF4\x90\x80\x80")) << "beyond U+10FFFF";
}

TEST(Unicode, RepairReplacesEachInvalidByte) {
    EXPECT_EQ(repair("a\x80"
                     "b"),
              "a\uFFFDb");
    EXPECT_EQ(repair("\xE2\x82"), "\uFFFD\uFFFD");
    EXPECT_EQ(repair("caf\u00E9"), "caf\u00E9") << "valid text is kept as it is";
}

TEST(Unicode, NfcComposes) {
    EXPECT_EQ(unwrap(nfc("e\u0301te\u0301")), "\u00E9t\u00E9");
    EXPECT_EQ(unwrap(nfc("\u00E9t\u00E9")), "\u00E9t\u00E9");
    EXPECT_EQ(unwrap(nfc("\uFB01")), "\uFB01") << "NFC keeps ligatures; only fold() decomposes them";
    EXPECT_EQ(unwrap(nfc("")), "");
}

TEST(Unicode, FoldMakesTheWaysOfWritingOneTextEqual) {
    EXPECT_EQ(unwrap(fold("Stra\u00DFe")), "strasse");
    EXPECT_EQ(unwrap(fold("STRASSE")), "strasse");
    EXPECT_EQ(unwrap(fold("Caf\u00E9")), "cafe") << "accents, which captions often lose";
    EXPECT_EQ(unwrap(fold("Cafe\u0301")), "cafe") << "decomposed accents too";
    EXPECT_EQ(unwrap(fold("\uFB01ne")), "fine") << "ligatures";
    EXPECT_EQ(unwrap(fold("\uFF21\uFF22\uFF23")), "abc") << "fullwidth letters";
    EXPECT_EQ(unwrap(fold("don\u2019t")), "don't");
    EXPECT_EQ(unwrap(fold("don\u2018t")), "don't");
    EXPECT_EQ(unwrap(fold("don\u02BCt")), "don't");
}

TEST(Unicode, NormalizingRefusesInvalidUtf8) {
    for (const auto& result : {nfc("\x80"), fold("ok \xFF")}) {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), Errc::invalid_argument);
    }
}

TEST(Unicode, CodePointsKnowWhereTheyAre) {
    const std::vector<CodePoint> points = code_points("a\u00E9\u20AC\U0001F600");
    ASSERT_EQ(points.size(), 4U);
    EXPECT_EQ(points[0].value, U'a');
    EXPECT_EQ(points[1].value, U'\u00E9');
    EXPECT_EQ(points[2].value, U'\u20AC');
    EXPECT_EQ(points[3].value, U'\U0001F600');
    EXPECT_EQ(points[1].begin, 1U);
    EXPECT_EQ(points[1].end, 3U);
    EXPECT_EQ(points[3].begin, 6U);
    EXPECT_EQ(points[3].end, 10U);
}

TEST(Unicode, AnInvalidByteIsOneReplacementCharacter) {
    const std::vector<CodePoint> points = code_points("a\xE2\x82z");
    ASSERT_EQ(points.size(), 4U);
    EXPECT_EQ(points[1].value, U'\uFFFD');
    EXPECT_EQ(points[1].begin, 1U);
    EXPECT_EQ(points[1].end, 2U);
    EXPECT_EQ(points[2].value, U'\uFFFD');
    EXPECT_EQ(points[3].value, U'z');
}

TEST(Unicode, ClassifiesCharacters) {
    EXPECT_EQ(classify(U'a'), CharClass::letter);
    EXPECT_EQ(classify(U'\u00C9'), CharClass::letter);
    EXPECT_EQ(classify(U'\u4E2D'), CharClass::letter) << "letters of any script";
    EXPECT_EQ(classify(U'\u0301'), CharClass::mark);
    EXPECT_EQ(classify(U'7'), CharClass::digit);
    EXPECT_EQ(classify(U'\u0667'), CharClass::digit);
    EXPECT_EQ(classify(U'.'), CharClass::punctuation);
    EXPECT_EQ(classify(U'\u2014'), CharClass::punctuation);
    EXPECT_EQ(classify(U'$'), CharClass::symbol);
    EXPECT_EQ(classify(U'%'), CharClass::punctuation);
    EXPECT_EQ(classify(U'\u00B2'), CharClass::symbol) << "a number that is no decimal digit";
    EXPECT_EQ(classify(U'\u216B'), CharClass::symbol) << "a Roman numeral";
    EXPECT_EQ(classify(U' '), CharClass::space);
    EXPECT_EQ(classify(U'\u00A0'), CharClass::space);
    EXPECT_EQ(classify(U'\t'), CharClass::space);
    EXPECT_EQ(classify(U'\n'), CharClass::space);
    EXPECT_EQ(classify(U'\r'), CharClass::space);
    EXPECT_EQ(classify(U'\u2029'), CharClass::space);
    EXPECT_EQ(classify(U'\0'), CharClass::other);
    EXPECT_EQ(classify(U'\x1b'), CharClass::other);
    EXPECT_EQ(classify(U'\u200B'), CharClass::other) << "a format character";
}

TEST(Unicode, KnowsCase) {
    EXPECT_TRUE(is_upper(U'A'));
    EXPECT_TRUE(is_upper(U'\u01C5')) << "titlecase";
    EXPECT_FALSE(is_upper(U'a'));
    EXPECT_TRUE(is_lower(U'a'));
    EXPECT_FALSE(is_lower(U'A'));
    EXPECT_FALSE(is_upper(U'\u4E2D'));
    EXPECT_FALSE(is_lower(U'\u4E2D')) << "a script without case";
    EXPECT_FALSE(is_lower(U'1'));
}

TEST(Unicode, KnowsOpeningAndClosingPunctuation) {
    for (const char32_t opening : {U'(', U'[', U'{', U'\u201C', U'\u2018', U'\u00AB'}) {
        EXPECT_TRUE(is_opening(opening)) << static_cast<unsigned>(opening);
        EXPECT_FALSE(is_closing(opening)) << static_cast<unsigned>(opening);
    }
    for (const char32_t closing : {U')', U']', U'}', U'\u201D', U'\u2019', U'\u00BB'}) {
        EXPECT_TRUE(is_closing(closing)) << static_cast<unsigned>(closing);
        EXPECT_FALSE(is_opening(closing)) << static_cast<unsigned>(closing);
    }
    for (const char32_t neither : {U'"', U'\'', U'.', U'a'}) {
        EXPECT_FALSE(is_opening(neither)) << static_cast<unsigned>(neither);
        EXPECT_FALSE(is_closing(neither)) << static_cast<unsigned>(neither);
    }
}

TEST(Unicode, ReadsTheDigitsOfAnyScript) {
    EXPECT_EQ(digit_value(U'0'), 0);
    EXPECT_EQ(digit_value(U'7'), 7);
    EXPECT_EQ(digit_value(U'\u0667'), 7) << "Arabic-Indic";
    EXPECT_EQ(digit_value(U'\u096D'), 7) << "Devanagari, whose zero is U+0966";
    EXPECT_EQ(digit_value(U'\uFF17'), 7) << "fullwidth";
    EXPECT_EQ(digit_value(U'\U0001D7D7'), 9) << "mathematical bold, the first of five adjacent sets";
    EXPECT_EQ(digit_value(U'\U0001D7D8'), 0) << "double-struck: a set right after another";
    EXPECT_EQ(digit_value(U'\U0001D7E1'), 9);
    EXPECT_EQ(digit_value(U'a'), -1);
    EXPECT_EQ(digit_value(U'\u00B2'), -1) << "a superscript is not a decimal digit";
}

}  // namespace
}  // namespace rr::text
