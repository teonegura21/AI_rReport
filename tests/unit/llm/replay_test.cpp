#include <rr/llm/replay.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/core/file_io.hpp>
#include <rr/core/strings.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

#include "scripted_provider.hpp"

namespace rr::llm {
namespace {

using testing::answer;
using testing::answers;
using testing::ask;
using ::testing::HasSubstr;
using testing::ScriptedProvider;
using testkit::unwrap;

constexpr ProviderCaps kCaps{.json_schema = true};

TEST(Replay, RecordsAnswersAndReplaysThemWithoutAModel) {
    const testkit::TempDir dir;
    const Recordings recordings{dir / "recordings"};
    ChatResponse live = answer("the capital is Paris");
    live.first().reasoning = "The user asks about France.";
    live.usage.cached_prompt_tokens = 4;
    const ScriptedProvider model{{live}, kCaps, "runtime"};
    const RecordingProvider recording{model, recordings};
    ASSERT_OK(recording.chat(ask("capital of France?"), {}, {}));

    const std::string key = request_key("runtime", ask("capital of France?"));
    const std::string file = unwrap(read_file(recordings.path_for(key)));
    EXPECT_THAT(file, HasSubstr("capital of France?")) << "the request is kept for people to read";
    EXPECT_THAT(file, HasSubstr("\n  ")) << "and pretty-printed, so its diff reviews well";

    const ReplayProvider replay{"runtime", kCaps, recordings};
    std::string streamed;
    std::string thought;
    const StreamSink sink{.on_content = [&](std::size_t, std::string_view text) { streamed += text; },
                          .on_reasoning = [&](std::size_t, std::string_view text) { thought += text; }};
    const ChatResponse replayed = unwrap(replay.chat(ask("capital of France?"), sink, {}));
    EXPECT_EQ(replayed.choices, live.choices) << "content, reasoning, finish and tokens";
    EXPECT_EQ(replayed.usage, live.usage);
    EXPECT_EQ(replayed.model, live.model);
    EXPECT_EQ(streamed, live.first().content);
    EXPECT_EQ(thought, live.first().reasoning);
    EXPECT_EQ(replay.name(), "runtime");
    EXPECT_TRUE(replay.caps().json_schema);
}

TEST(Replay, EveryChoiceIsRecordedAndReplayedToItsIndex) {
    const testkit::TempDir dir;
    const Recordings recordings{dir / "recordings"};
    ChatRequest drafts = ask("three drafts, please");
    drafts.n = 3;
    drafts.temperature = 0.7;
    ChatResponse live = answers({"first draft", "second draft", "third draft"});
    live.choices[2].finish = FinishReason::length;
    const ScriptedProvider model{{live}, kCaps, "runtime"};
    ASSERT_OK(RecordingProvider(model, recordings).chat(drafts, {}, {}));

    std::vector<std::string> streamed(3);
    const StreamSink sink{
        .on_content = [&](std::size_t choice, std::string_view text) { streamed.at(choice) += text; }};
    const ChatResponse replayed = unwrap(ReplayProvider("runtime", kCaps, recordings).chat(drafts, sink, {}));
    EXPECT_EQ(replayed.choices, live.choices);
    EXPECT_EQ(replayed.usage, live.usage);
    EXPECT_EQ(streamed, (std::vector<std::string>{"first draft", "second draft", "third draft"}))
        << "each choice reaches the sink under its own index";
}

TEST(Replay, ARecordingInAnOlderFormatIsRefusedNotGuessedAt) {
    const testkit::TempDir dir;
    const Recordings recordings{dir / "recordings"};
    const std::string key = request_key("runtime", ask("hi"));
    std::filesystem::create_directories(dir / "recordings");
    // Format 1: one answer, no "format" field.
    ASSERT_OK(write_file_atomically(
        recordings.path_for(key),
        json::dump(json::Json{
            {"key", key},
            {"provider", "runtime"},
            {"request", to_json(ask("hi"))},
            {"response",
             {{"content", "hello"},
              {"reasoning", ""},
              {"finish_reason", "stop"},
              {"model", "m"},
              {"usage", {{"prompt_tokens", 1}, {"completion_tokens", 1}, {"cached_prompt_tokens", 0}}}}}})));
    const auto replayed = ReplayProvider("runtime", kCaps, recordings).chat(ask("hi"), {}, {});
    ASSERT_ERRC(replayed, Errc::integrity);
    EXPECT_THAT(replayed.error().message(), HasSubstr("format 1"));
    EXPECT_THAT(replayed.error().message(), HasSubstr("record the call again"));
}

TEST(Replay, AnUnrecordedCallFailsAndNamesItsKey) {
    const testkit::TempDir dir;
    const ReplayProvider replay{"runtime", kCaps, Recordings{dir.path()}};
    const auto missing = replay.chat(ask("never asked"), {}, {});
    ASSERT_ERRC(missing, Errc::not_found);
    EXPECT_THAT(missing.error().message(), HasSubstr(request_key("runtime", ask("never asked"))));
    EXPECT_THAT(missing.error().message(), HasSubstr("record it first"));
}

TEST(Replay, FailuresAreNotRecorded) {
    const testkit::TempDir dir;
    const Recordings recordings{dir / "recordings"};
    const ScriptedProvider model{{fail(Errc::timeout, "slow"), answer("ok")}, kCaps, "runtime"};
    const RecordingProvider recording{model, recordings};
    EXPECT_ERRC(recording.chat(ask("hi"), {}, {}), Errc::timeout);
    EXPECT_FALSE(std::filesystem::exists(recordings.path_for(request_key("runtime", ask("hi")))))
        << "a replayed failure would make a test depend on luck";
}

TEST(Replay, RefusesFilesThatAreNotThisCallsRecording) {
    const testkit::TempDir dir;
    const Recordings recordings{dir.path()};
    ASSERT_OK(recordings.store("runtime", ask("one"), answer("1")));
    const std::string one = request_key("runtime", ask("one"));
    const std::string two = request_key("runtime", ask("two"));
    // A recording copied under another call's name must not answer that call.
    std::filesystem::copy_file(recordings.path_for(one), recordings.path_for(two));
    const auto copied = recordings.load(two);
    ASSERT_ERRC(copied, Errc::integrity);
    EXPECT_THAT(copied.error().message(), HasSubstr("records the call " + one));

    ASSERT_OK(write_file_atomically(recordings.path_for(one), "{ not json"));
    EXPECT_ERRC(recordings.load(one), Errc::integrity);
}

TEST(Replay, ACancelledCallIsCancelled) {
    const testkit::TempDir dir;
    const Recordings recordings{dir.path()};
    ASSERT_OK(recordings.store("runtime", ask("hi"), answer("hello")));
    const ReplayProvider replay{"runtime", kCaps, recordings};
    // Non-const: request_stop() is a non-const member in the C++ standard.
    std::stop_source stop;  // NOLINT(misc-const-correctness)
    stop.request_stop();
    EXPECT_ERRC(replay.chat(ask("hi"), {}, stop.get_token()), Errc::cancelled);
}

}  // namespace
}  // namespace rr::llm
