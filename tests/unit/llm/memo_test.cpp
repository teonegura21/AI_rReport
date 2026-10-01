#include <rr/llm/memo.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

#include "scripted_provider.hpp"

namespace rr::llm {
namespace {

using namespace std::chrono_literals;
using testing::answer;
using testing::ask;
using testing::ScriptedProvider;
using testkit::unwrap;

// A memo in memory, which can be told to refuse writes or fail reads.
class MemoryStore final : public MemoStore {
public:
    [[nodiscard]] Result<std::optional<ChatResponse>> find(std::string_view key) const override {
        const std::lock_guard lock(mutex_);
        if (fail_reads_) {
            return fail(Errc::io, "the memo cannot be read");
        }
        const auto found = kept_.find(std::string{key});
        return found == kept_.end() ? std::optional<ChatResponse>{} : std::optional{found->second};
    }
    [[nodiscard]] Result<void> keep(std::string_view key, const ChatResponse& response) override {
        const std::lock_guard lock(mutex_);
        if (refuse_writes_) {
            return fail(Errc::io, "the disk is full");
        }
        kept_.try_emplace(std::string{key}, response);
        return ok();
    }
    void refuse_writes() { refuse_writes_ = true; }
    void fail_reads() { fail_reads_ = true; }
    [[nodiscard]] std::size_t size() const {
        const std::lock_guard lock(mutex_);
        return kept_.size();
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, ChatResponse> kept_;
    bool refuse_writes_ = false;
    bool fail_reads_ = false;
};

ModelFingerprint model_file(std::string checksum) {
    return [checksum = std::move(checksum)](std::string_view) -> Result<std::string> { return checksum; };
}

// A sampled request, with a seed as a job's are.
ChatRequest sample(std::string text, std::int64_t seed) {
    ChatRequest request = ask(std::move(text));
    request.temperature = 0.7;
    request.seed = seed;
    return request;
}

TEST(Memo, AnAnswerGivenOnceIsGivenFromMemoryAfterwards) {
    const ScriptedProvider model{{answer("Paris")}};
    MemoryStore store;
    const MemoProvider memo{model, store, model_file("sha-q4")};
    ASSERT_OK(memo.chat(sample("capital of France?", 7), {}, {}));

    std::string streamed;
    const StreamSink sink{.on_content = [&](std::size_t, std::string_view text) { streamed += text; }};
    const ChatResponse again = unwrap(memo.chat(sample("capital of France?", 7), sink, {}));
    EXPECT_EQ(again.first().content, "Paris");
    EXPECT_EQ(streamed, "Paris") << "a remembered answer still reaches the sink, whole";
    EXPECT_EQ(again.duration, 0ms);
    EXPECT_EQ(model.requests().size(), 1U) << "the model was asked once";
    EXPECT_EQ(memo.remembered(), 1);
    EXPECT_EQ(memo.made(), 1);
}

TEST(Memo, TheServedModelIsPartOfTheKey) {
    const ScriptedProvider model{{answer("from Q4"), answer("from Q8")}};
    MemoryStore store;
    ASSERT_OK(MemoProvider(model, store, model_file("sha-q4")).chat(sample("x", 7), {}, {}));
    const ChatResponse other =
        unwrap(MemoProvider(model, store, model_file("sha-q8")).chat(sample("x", 7), {}, {}));
    EXPECT_EQ(other.first().content, "from Q8") << "another model behind the same role is another call";
    EXPECT_EQ(model.requests().size(), 2U);
}

TEST(Memo, ARequestThatMaySampleNeedsASeed) {
    const ScriptedProvider model{{answer("greedy")}};
    MemoryStore store;
    const MemoProvider memo{model, store, model_file("m")};
    ChatRequest unseeded = ask("x");
    EXPECT_ERRC(memo.chat(unseeded, {}, {}), Errc::invalid_argument) << "temperature unset: servers sample";
    unseeded.temperature = 0.7;
    EXPECT_ERRC(memo.chat(unseeded, {}, {}), Errc::invalid_argument);
    EXPECT_TRUE(model.requests().empty()) << "refused before any call";

    ChatRequest greedy = ask("x");
    greedy.temperature = 0.0;
    EXPECT_OK(memo.chat(greedy, {}, {})) << "greedy decoding has no dice to fix";
}

TEST(Memo, SamplesWithTheirOwnSeedsAreTheirOwnCalls) {
    const ScriptedProvider model{{answer("sample 0"), answer("sample 1")}};
    MemoryStore store;
    const MemoProvider memo{model, store, model_file("m")};
    EXPECT_EQ(unwrap(memo.chat(sample("label this", derived_seed(0, "atomize", "S1-S25", 0)), {}, {}))
                  .first()
                  .content,
              "sample 0");
    EXPECT_EQ(unwrap(memo.chat(sample("label this", derived_seed(0, "atomize", "S1-S25", 1)), {}, {}))
                  .first()
                  .content,
              "sample 1")
        << "without its own seed, the second sample would get the first one's answer";
    EXPECT_EQ(unwrap(memo.chat(sample("label this", derived_seed(0, "atomize", "S1-S25", 1)), {}, {}))
                  .first()
                  .content,
              "sample 1");
    EXPECT_EQ(model.requests().size(), 2U);
}

TEST(Memo, OnlySuccessesAreRemembered) {
    const ScriptedProvider model{{fail(Errc::unavailable, "engine restarting"), answer("ok")}};
    MemoryStore store;
    const MemoProvider memo{model, store, model_file("m")};
    EXPECT_ERRC(memo.chat(sample("x", 7), {}, {}), Errc::unavailable);
    EXPECT_EQ(store.size(), 0U);
    EXPECT_EQ(unwrap(memo.chat(sample("x", 7), {}, {})).first().content, "ok") << "asked again, not replayed";
    EXPECT_EQ(store.size(), 1U);
}

TEST(Memo, AnAnswerOutlivesAFailureToKeepIt) {
    const ScriptedProvider model{{answer("paid for"), answer("asked again")}};
    MemoryStore store;
    store.refuse_writes();
    const MemoProvider memo{model, store, model_file("m")};
    EXPECT_EQ(unwrap(memo.chat(sample("x", 7), {}, {})).first().content, "paid for")
        << "losing a memo entry costs a repeated call, never the answer";
    EXPECT_EQ(unwrap(memo.chat(sample("x", 7), {}, {})).first().content, "asked again");
}

TEST(Memo, AMemoThatCannotBeReadOrAModelThatCannotBeNamedFailsTheCall) {
    const ScriptedProvider model{{answer("unasked")}};
    MemoryStore store;
    store.fail_reads();
    EXPECT_ERRC(MemoProvider(model, store, model_file("m")).chat(sample("x", 7), {}, {}), Errc::io)
        << "a fault to report, not a reason to pay for the call again unnoticed";

    MemoryStore fine;
    const ModelFingerprint unknown = [](std::string_view model_name) -> Result<std::string> {
        return fail(Errc::not_found, std::format("no engine serves '{}'", model_name));
    };
    EXPECT_ERRC(MemoProvider(model, fine, unknown).chat(sample("x", 7), {}, {}), Errc::not_found);
    EXPECT_TRUE(model.requests().empty());
}

TEST(Memo, SeedsAreDerivedSoThatAResumeAsksTheSameQuestions) {
    const std::int64_t seed = derived_seed(0, "atomize", "S1-S25", 0);
    EXPECT_EQ(derived_seed(0, "atomize", "S1-S25", 0), seed) << "deterministic";
    EXPECT_EQ(derived_seed(0, "atomize", "S1-S25", 2), seed + 2)
        << "a mission's samples are consecutive: a request for n choices draws them";
    EXPECT_NE(derived_seed(1, "atomize", "S1-S25", 0), seed) << "the replicate's salt changes the dice";
    EXPECT_NE(derived_seed(0, "write", "S1-S25", 0), seed);
    EXPECT_NE(derived_seed(0, "atomize", "S26-S50", 0), seed);
    EXPECT_GE(seed, 0);
    EXPECT_LT(seed, std::int64_t{1} << 31) << "far below 2^32 - 1, llama-server's 'pick one at random'";
    // Pinned, and computed independently (Python's blake3 over the same
    // canonical JSON): changing the derivation would orphan every job's memo
    // and break the eval's common random numbers between versions.
    EXPECT_EQ(seed, 814'433'917);
    // This one's hash has bit 31 set, which the mask must clear (unmasked:
    // 3'659'563'847, beyond 2^31).
    EXPECT_EQ(derived_seed(0, "atomize", "S51-S75", 0), 1'512'080'199);
}

}  // namespace
}  // namespace rr::llm
