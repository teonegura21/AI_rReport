#include <rr/net/sse.hpp>

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

namespace rr::net {

// Readable failures. Found by argument-dependent lookup, so it lives in
// SseEvent's namespace rather than the anonymous one below.
void PrintTo(const SseEvent& event, std::ostream* out) {
    *out << "{type: " << event.type << ", data: \"" << event.data << "\", id: " << event.id << "}";
}

namespace {

using namespace std::chrono_literals;

using Events = std::vector<SseEvent>;

// Parses `chunks` in order; all must succeed.
Events parse(const std::vector<std::string_view>& chunks, SseParser& parser) {
    Events events;
    for (const auto chunk : chunks) {
        const auto fed = parser.feed(chunk, [&](const SseEvent& event) -> Result<void> {
            events.push_back(event);
            return ok();
        });
        EXPECT_OK(fed);
    }
    return events;
}

Events parse(const std::vector<std::string_view>& chunks) {
    SseParser parser;
    return parse(chunks, parser);
}

SseEvent message(std::string data, std::string id = {}) {
    return SseEvent{.type = "message", .data = std::move(data), .id = std::move(id)};
}

TEST(SseParser, DispatchesOnABlankLine) {
    EXPECT_EQ(parse({"data: hello\n\n"}), (Events{message("hello")}));
    EXPECT_EQ(parse({"data: hello\n"}), Events{}) << "an unfinished event is not dispatched";
}

TEST(SseParser, JoinsDataLinesAndRemovesOnlyOneSpace) {
    EXPECT_EQ(parse({"data: first\ndata:second\ndata:  third\n\n"}),
              (Events{message("first\nsecond\n third")}));
    EXPECT_EQ(parse({"data\n\n"}), (Events{message("")})) << "a bare field name means an empty value";
}

TEST(SseParser, NamesEventsAndKeepsTheLastIdAcrossEvents) {
    const Events events = parse({"event: message_delta\nid: 7\ndata: {}\n\ndata: next\n\n"});
    EXPECT_EQ(events,
              (Events{SseEvent{.type = "message_delta", .data = "{}", .id = "7"}, message("next", "7")}));
}

TEST(SseParser, IgnoresCommentsUnknownFieldsAndEventsWithoutData) {
    EXPECT_EQ(parse({": keep-alive\n\nevent: ping\n\nfoo: bar\ndata: real\n\n"}), (Events{message("real")}));
}

TEST(SseParser, IgnoresIdsContainingNul) {
    SseParser parser;
    const std::string stream = std::string{"id: 1\ndata: a\n\nid: 2"} + '\0' + "x\ndata: b\n\n";
    EXPECT_EQ(parse({stream}, parser), (Events{message("a", "1"), message("b", "1")}));
    EXPECT_EQ(parser.last_event_id(), "1");
}

TEST(SseParser, AcceptsEveryLineEndingEvenSplitAcrossChunks) {
    EXPECT_EQ(parse({"data: crlf\r\n\r\ndata: cr\r\rdata: lf\n\n"}),
              (Events{message("crlf"), message("cr"), message("lf")}));
    // The CR ends one chunk and its LF begins the next: still one line end,
    // so no blank line appears and nothing is dispatched early.
    EXPECT_EQ(parse({"data: a\r", "\ndata: b\r", "\n\r", "\n"}), (Events{message("a\nb")}));
}

TEST(SseParser, IgnoresAByteOrderMarkOnlyAtTheStart) {
    EXPECT_EQ(parse({"\xEF\xBB\xBF"
                     "data: x\n\n"}),
              (Events{message("x")}));
    EXPECT_EQ(parse({"\xEF", "\xBB",
                     "\xBF"
                     "data: x\n\n"}),
              (Events{message("x")}))
        << "split across chunks";
    // Later on, a BOM is an ordinary character: it becomes part of the field
    // name ("\uFEFFdata"), which is unknown, so that line is ignored.
    EXPECT_EQ(parse({"data: x\n\n\xEF\xBB\xBF"
                     "data: y\n\n"}),
              (Events{message("x")}));
}

TEST(SseParser, ReadsRetryOnlyWhenItIsAllDigits) {
    SseParser parser;
    (void)parse({"retry: 3000\n\n"}, parser);
    EXPECT_EQ(parser.retry(), 3000ms);
    (void)parse({"retry: 5s\nretry: -1\nretry: \n\n"}, parser);
    EXPECT_EQ(parser.retry(), 3000ms) << "invalid values are ignored";
}

TEST(SseParser, ChunkBoundariesNeverChangeTheResult) {
    const std::string stream =
        "\xEF\xBB\xBF: comment\r\nevent: delta\r\nid: 42\r\ndata: {\"text\": \"a\"}\r\n\r\n"
        "data: line one\rdata: line two\r\rretry: 10\ndata: [DONE]\n\ndata: unfinished";
    const Events whole = parse({stream});
    ASSERT_EQ(whole.size(), 3U);
    // Every split point, and byte-at-a-time, must agree with the whole.
    for (std::size_t split = 0; split <= stream.size(); ++split) {
        EXPECT_EQ(parse({std::string_view{stream}.substr(0, split), std::string_view{stream}.substr(split)}),
                  whole)
            << "split at " << split;
    }
    SseParser bytewise;
    Events one_by_one;
    for (const char byte : stream) {
        ASSERT_OK(bytewise.feed(std::string_view{&byte, 1}, [&](const SseEvent& event) -> Result<void> {
            one_by_one.push_back(event);
            return ok();
        }));
    }
    EXPECT_EQ(one_by_one, whole);
}

TEST(SseParser, EnforcesItsSizeLimit) {
    SseParser endless_line{64};
    EXPECT_ERRC(endless_line.feed(std::string(100, 'x'), nullptr), Errc::invalid_argument);
    SseParser endless_event{64};
    EXPECT_ERRC(endless_event.feed("data: 0123456789012345678901234567890123456789\n"
                                   "data: 0123456789012345678901234567890123456789\n",
                                   nullptr),
                Errc::invalid_argument);
}

TEST(SseParser, AFailingCallbackStopsParsing) {
    SseParser parser;
    int delivered = 0;
    const auto result =
        parser.feed("data: 1\n\ndata: 2\n\ndata: 3\n\n", [&](const SseEvent&) -> Result<void> {
            ++delivered;
            return fail(Errc::model_output_invalid, "bad JSON");
        });
    EXPECT_ERRC(result, Errc::model_output_invalid);
    EXPECT_EQ(delivered, 1);
}

TEST(SseFormat, RoundTripsThroughTheParser) {
    for (const std::string_view data :
         {"plain", "", "two\nlines", "trailing\n", "windows\r\nline", "old\rmac"}) {
        std::string expected{data};
        // CR and CRLF come back as LF: SSE has no way to carry them.
        for (std::size_t at = expected.find('\r'); at != std::string::npos; at = expected.find('\r', at)) {
            if (at + 1 < expected.size() && expected[at + 1] == '\n') {
                expected.erase(at, 1);
            } else {
                expected[at] = '\n';
            }
        }
        EXPECT_EQ(parse({format_sse_event(data, "chunk", "9")}),
                  (Events{SseEvent{.type = "chunk", .data = expected, .id = "9"}}))
            << "data: \"" << data << "\"";
    }
    EXPECT_EQ(parse({format_sse_comment("keep\nalive"), format_sse_event("x")}), (Events{message("x")}));
    EXPECT_EQ(format_sse_comment("keep\nalive"), ": keep alive\n") << "one line, and no blank line after it";
}

TEST(SseLineTracker, OnlyALineFeedStartsALine) {
    SseLineTracker tracker;
    EXPECT_TRUE(tracker.at_line_start()) << "nothing sent yet";
    tracker.feed("data: x");
    EXPECT_FALSE(tracker.at_line_start());
    tracker.feed("\r");
    EXPECT_FALSE(tracker.at_line_start()) << "a CR may be the first half of a CRLF";
    tracker.feed("\n");
    EXPECT_TRUE(tracker.at_line_start());
    tracker.feed("");
    EXPECT_TRUE(tracker.at_line_start()) << "an empty write changes nothing";
    tracker.feed("data: y\n\n");
    EXPECT_TRUE(tracker.at_line_start());
}

// The property a relay relies on: comments inserted wherever the tracker
// allows, with a pause (a chance to insert) after every single byte, never
// change the events a client parses.
TEST(SseLineTracker, CommentsWhereverItAllowsNeverChangeTheEvents) {
    const std::vector<std::string_view> streams{
        "data: a\n\ndata: b\n\n",
        "event: delta\nid: 7\ndata: first\ndata: second\n\n",
        "data: crlf\r\ndata: lines\r\n\r\ndata: next\r\n\r\n",
        "data: mixed\r\n\ndata: ends\r\r",
        ": an upstream comment\ndata: z\n\n",
    };
    const std::string ping = format_sse_comment("keep-alive");
    for (const std::string_view stream : streams) {
        std::string relayed;
        SseLineTracker tracker;
        int inserted = 0;
        for (std::size_t at = 0; at <= stream.size(); ++at) {  // every position, the end included
            if (tracker.at_line_start()) {
                relayed += ping;
                tracker.feed(ping);
                ++inserted;
            }
            if (at < stream.size()) {
                relayed.push_back(stream[at]);
                tracker.feed(stream.substr(at, 1));
            }
        }
        EXPECT_GT(inserted, 1) << stream;
        EXPECT_EQ(parse({relayed}), parse({stream})) << "relayed: " << relayed;
    }
}

TEST(SseLineTracker, AnEventFitsOnlyBetweenEvents) {
    SseLineTracker tracker;
    const auto state = [&](std::string_view bytes) {
        tracker.feed(bytes);
        return std::pair{tracker.at_line_start(), tracker.between_events()};
    };
    using State = std::pair<bool, bool>;
    EXPECT_EQ(state(""), (State{true, true})) << "nothing sent yet";
    EXPECT_EQ(state("data: x\n"), (State{true, false})) << "a comment fits, an event would merge";
    EXPECT_EQ(state("\n"), (State{true, true}));
    EXPECT_EQ(state(": comment\n"), (State{true, true})) << "comments leave nothing pending";
    EXPECT_EQ(state("event: e\n"), (State{true, false}));
    EXPECT_EQ(state("data: y\r\n\r\n"), (State{true, true}));
    EXPECT_EQ(state("id: 7\n"), (State{true, false})) << "any field line counts: conservative";
    EXPECT_EQ(state("\r"), (State{false, false})) << "a CR may be half of a CRLF";
    EXPECT_EQ(state("\n"), (State{true, true})) << "it was: CR and LF made one blank line";
    EXPECT_EQ(state("data: z\r\r"), (State{false, false})) << "a bare CR could still be followed by an LF";
    EXPECT_EQ(state("data: z"), (State{false, false}));
}

// The same property for whole events: inserted wherever the tracker allows,
// they arrive as events of their own, and every original event is unchanged.
TEST(SseLineTracker, EventsWhereverItAllowsLeaveTheOthersIntact) {
    const std::vector<std::string_view> streams{
        "data: a\n\ndata: b\n\n",
        "event: delta\nid: 7\ndata: first\ndata: second\n\n",
        "data: crlf\r\ndata: lines\r\n\r\ndata: next\r\n\r\n",
        "data: mixed\r\n\ndata: ends\r\r",
        ": an upstream comment\ndata: z\n\n",
    };
    const std::string inserted = format_sse_event("inserted");
    for (const std::string_view stream : streams) {
        std::string relayed;
        SseLineTracker tracker;
        int insertions = 0;
        for (std::size_t at = 0; at <= stream.size(); ++at) {  // every position, the end included
            if (tracker.between_events()) {
                relayed += inserted;
                tracker.feed(inserted);
                ++insertions;
            }
            if (at < stream.size()) {
                relayed.push_back(stream[at]);
                tracker.feed(stream.substr(at, 1));
            }
        }
        Events originals;
        int received = 0;
        for (const SseEvent& event : parse({relayed})) {
            if (event.data == "inserted") {
                ++received;
            } else {
                originals.push_back(event);
            }
        }
        EXPECT_GT(insertions, 1) << stream;
        EXPECT_EQ(received, insertions) << "relayed: " << relayed;
        EXPECT_EQ(originals, parse({stream})) << "relayed: " << relayed;
    }
}

// Why a CR alone is not a line start: a comment between the CR and the LF of
// a CRLF turns that LF into a blank line, and the event ends early.
TEST(SseLineTracker, ACommentInsideACrlfWouldSplitTheEvent) {
    const std::string_view stream = "data: a\r\ndata: b\r\n\r\n";
    const std::string naive = "data: a\r" + format_sse_comment("keep-alive") + "\ndata: b\r\n\r\n";
    EXPECT_EQ(parse({stream}), (Events{message("a\nb")}));
    EXPECT_EQ(parse({naive}), (Events{message("a"), message("b")})) << "what the tracker prevents";
}

}  // namespace
}  // namespace rr::net
