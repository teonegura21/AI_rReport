#include <rr/net/http.hpp>

#include <chrono>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

namespace rr::net {
namespace {

using namespace std::chrono_literals;
using ::testing::HasSubstr;

TEST(Headers, CompareNamesCaseInsensitivelyAndKeepDuplicates) {
    Headers headers{{"Content-Type", "application/json"}};
    headers.add("Set-Cookie", "a=1");
    headers.add("set-cookie", "b=2");
    EXPECT_EQ(headers.get("content-type"), "application/json");
    EXPECT_EQ(headers.get_all("SET-COOKIE"), (std::vector<std::string_view>{"a=1", "b=2"}));
    EXPECT_FALSE(headers.contains("Authorization"));

    headers.set("SET-COOKIE", "c=3");
    EXPECT_EQ(headers.get_all("set-cookie"), (std::vector<std::string_view>{"c=3"}));
    headers.remove("Content-TYPE");
    EXPECT_EQ(headers.get("Content-Type"), std::nullopt);
    EXPECT_EQ(headers.size(), 1U);
}

TEST(Methods, HaveTheirWireNames) {
    EXPECT_EQ(to_string(Method::get), "GET");
    EXPECT_EQ(to_string(Method::del), "DELETE");
    EXPECT_EQ(to_string(Method::patch), "PATCH");
}

// Every error class, in declaration order.
std::vector<Errc> all_error_classes() {
    std::vector<Errc> classes;
    for (int index = 0;; ++index) {
        const auto code = static_cast<Errc>(index);
        classes.push_back(code);
        if (code == Errc::internal) {
            return classes;
        }
    }
}

TEST(ErrorBodies, CarryEveryErrorClassAcrossTheWireExactly) {
    // Status codes alone are lossy (io and internal are both 500); the
    // structured body is what keeps the class intact.
    for (const Errc code : all_error_classes()) {
        const Error original = Error{code, "what went wrong"}.with_context("while storing the blob");
        const Error received = error_from_response(http_status_for(code), Headers{}, error_body(original));
        EXPECT_EQ(received.code(), code) << to_string(code);
        EXPECT_EQ(received.message(), "what went wrong");
        ASSERT_EQ(received.context().size(), 1U);
        EXPECT_EQ(received.context()[0], "while storing the blob");
    }
}

TEST(ErrorBodies, CarryTheRetryHint) {
    const Error original = Error{Errc::rate_limited, "slow down"}.with_retry_after(2500ms);
    const Error received = error_from_response(429, Headers{}, error_body(original));
    EXPECT_EQ(received.retry_after(), 2500ms);
    // A Retry-After header, when present, is what the server means now.
    const Error from_header = error_from_response(429, Headers{{"Retry-After", "7"}}, error_body(original));
    EXPECT_EQ(from_header.retry_after(), 7000ms);
}

TEST(ErrorBodies, OtherApisAreClassifiedByStatusAndKeepTheirMessage) {
    // OpenAI-style: a foreign "code" is not one of ours; the status decides.
    const Error openai = error_from_response(
        400, Headers{},
        R"({"error": {"message": "context too long", "type": "invalid_request_error", "code": "x"}})");
    EXPECT_EQ(openai.code(), Errc::invalid_argument);
    EXPECT_THAT(openai.message(), HasSubstr("context too long"));

    // Anthropic-style overload: transient, so it will be retried.
    const Error anthropic = error_from_response(
        529, Headers{},
        R"({"type": "error", "error": {"type": "overloaded_error", "message": "Overloaded"}})");
    EXPECT_EQ(anthropic.code(), Errc::unavailable);
    EXPECT_TRUE(anthropic.transient());

    // Not JSON at all: the start of the body, on one line.
    const Error html = error_from_response(502, Headers{}, "<html>\n<body>Bad Gateway</body>\n</html>");
    EXPECT_EQ(html.code(), Errc::unavailable);
    EXPECT_THAT(html.message(), HasSubstr("HTTP 502: <html> <body>Bad Gateway"));
}

// Once a stream's 200 is on the wire, errors travel as events.
TEST(ErrorEvents, DecodeOurShapeAndLlamaCpps) {
    const auto ours =
        error_from_event(error_body(Error{Errc::integrity, "hash mismatch"}.with_retry_after(3000ms)));
    ASSERT_TRUE(ours.has_value());
    EXPECT_EQ(ours->code(), Errc::integrity);
    EXPECT_EQ(ours->message(), "hash mismatch");
    EXPECT_EQ(ours->retry_after(), 3000ms);

    // llama-server's shape: the HTTP status the error stands for, as an integer.
    const auto too_long = error_from_event(
        R"({"error":{"code":400,"message":"the request exceeds the available context size","type":"exceed_context_size_error"}})");
    ASSERT_TRUE(too_long.has_value());
    EXPECT_EQ(too_long->code(), Errc::invalid_argument) << "not retried: the same request fails the same way";
    EXPECT_THAT(too_long->message(), HasSubstr("exceeds the available context size"));

    const auto mid_stream =
        error_from_event(R"({"error":{"code":500,"message":"boom","type":"server_error"}})");
    ASSERT_TRUE(mid_stream.has_value());
    EXPECT_TRUE(mid_stream->transient()) << "a server-side failure is retried, as a 5xx status would be";

    for (const std::string_view other :
         {"[DONE]", R"({"choices":[]})", R"({"error":"only a string"})", "not json", ""}) {
        EXPECT_FALSE(error_from_event(other).has_value()) << other;
    }
}

TEST(StatusMapping, ClassifiesCommonStatuses) {
    EXPECT_EQ(errc_for_http_status(400), Errc::invalid_argument);
    EXPECT_EQ(errc_for_http_status(401), Errc::permission_denied);
    EXPECT_EQ(errc_for_http_status(403), Errc::permission_denied);
    EXPECT_EQ(errc_for_http_status(404), Errc::not_found);
    EXPECT_EQ(errc_for_http_status(408), Errc::timeout);
    EXPECT_EQ(errc_for_http_status(409), Errc::conflict);
    EXPECT_EQ(errc_for_http_status(413), Errc::invalid_argument);
    EXPECT_EQ(errc_for_http_status(429), Errc::rate_limited);
    EXPECT_EQ(errc_for_http_status(500), Errc::unavailable) << "5xx are retried by convention";
    EXPECT_EQ(errc_for_http_status(501), Errc::unsupported);
    EXPECT_EQ(errc_for_http_status(503), Errc::unavailable);
    EXPECT_EQ(errc_for_http_status(504), Errc::timeout);
    EXPECT_EQ(http_status_for(Errc::not_found), 404);
    EXPECT_EQ(http_status_for(Errc::integrity), 422);
    EXPECT_EQ(http_status_for(Errc::rate_limited), 429);
}

TEST(RetryAfter, ParsesSecondsAndHttpDates) {
    EXPECT_EQ(parse_retry_after("120"), 120s);
    EXPECT_EQ(parse_retry_after(" 5 "), 5s);
    EXPECT_EQ(parse_retry_after("0"), 0ms);
    EXPECT_EQ(parse_retry_after("99999999999999999999"), std::chrono::hours{24 * 7})
        << "clamped, not overflowed";

    // 1994-11-06 08:49:37 UTC is 784111777 seconds after the epoch.
    const std::chrono::system_clock::time_point now{std::chrono::seconds{784111777 - 90}};
    EXPECT_EQ(parse_retry_after("Sun, 06 Nov 1994 08:49:37 GMT", now), 90s);
    EXPECT_EQ(parse_retry_after("Sun, 06 Nov 1994 08:49:37 GMT", now + 1h), 0ms) << "a past date means now";

    EXPECT_EQ(parse_retry_after(""), std::nullopt);
    EXPECT_EQ(parse_retry_after("-5"), std::nullopt);
    EXPECT_EQ(parse_retry_after("soon"), std::nullopt);
    EXPECT_EQ(parse_retry_after("Sun, 31 Feb 1994 08:49:37 GMT"), std::nullopt) << "no such day";
    EXPECT_EQ(parse_retry_after("Sunday, 06-Nov-94 08:49:37 GMT"), std::nullopt)
        << "obsolete formats are ignored";
}

TEST(UrlEncode, EscapesEverythingButUnreservedCharacters) {
    EXPECT_EQ(url_encode("abcXYZ019-._~"), "abcXYZ019-._~");
    EXPECT_EQ(url_encode("a b/c?d=é&"), "a%20b%2Fc%3Fd%3D%C3%A9%26");
    EXPECT_EQ(url_encode(""), "");
}

}  // namespace
}  // namespace rr::net
