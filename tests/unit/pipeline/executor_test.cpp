#include <rr/pipeline/executor.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/agents/army.hpp>
#include <rr/library/library.hpp>
#include <rr/llm/provider.hpp>
#include <rr/reason/tier.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

#include "fake_stage.hpp"

namespace rr::pipeline {
namespace {

using fakes::FakeStage;
using fakes::said;
using fakes::Script;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using testkit::unwrap;

class ExecutorTest : public ::testing::Test {
protected:
    // An executor of the stages named, in order, each played by its script.
    Executor executor(const std::vector<std::string>& names) {
        std::vector<std::unique_ptr<Stage>> stages;
        stages.reserve(names.size());
        for (const std::string& name : names) {
            stages.push_back(std::make_unique<FakeStage>(name, script(name)));
        }
        return unwrap(Executor::create(std::move(stages), clock_));
    }

    std::shared_ptr<Script> script(const std::string& name) {
        return scripts_.try_emplace(name, std::make_shared<Script>()).first->second;
    }

    library::Job job(std::int64_t salt = 0) { return unwrap(jobs_.create("talk.vtt", "max", salt)); }

    RunReport run(const Executor& executor, const library::Job& job, std::stop_token stop = {}) {
        return executor.run(job, store_,
                            StageContext{.stop = std::move(stop), .progress = [this](std::string_view line) {
                                             progress_.emplace_back(line);
                                         }});
    }

    [[nodiscard]] db::Database& database() { return database_; }
    [[nodiscard]] FakeClock& clock() { return clock_; }
    [[nodiscard]] library::StageStore& store() { return store_; }
    [[nodiscard]] std::vector<std::string>& progress() { return progress_; }

private:
    testkit::TempDir dir_;
    db::Database database_ = unwrap(library::open_database(dir_ / "library.db"));
    FakeClock clock_;
    library::Jobs jobs_{database_, clock_};
    library::StageStore store_{database_, clock_};
    std::map<std::string, std::shared_ptr<Script>, std::less<>> scripts_;
    std::vector<std::string> progress_;
};

TEST_F(ExecutorTest, EachStageRunsOnceAndIsThenFoundDone) {
    script("transcript")->inputs = {"source"};
    script("transcript")->findings = {"S2 was not punctuated"};
    const Executor stages = executor({"source", "transcript"});

    const RunReport first = run(stages, job());
    ASSERT_FALSE(first.error.has_value());
    ASSERT_EQ(first.stages.size(), 2U);
    EXPECT_FALSE(first.stages[0].cached);
    EXPECT_EQ(first.stages[1].findings, std::vector<std::string>{"S2 was not punctuated"});
    EXPECT_EQ(first.stages[1].artifact, json::content_hash(said("Hello.")).hex());
    EXPECT_EQ(script("transcript")->inputs_seen.at("source"), said("Hello."))
        << "a stage reads what came before";

    const library::Job other = job();
    const RunReport second = run(stages, other);
    ASSERT_FALSE(second.error.has_value());
    ASSERT_EQ(second.stages.size(), 2U);
    EXPECT_TRUE(second.stages[0].cached);
    EXPECT_TRUE(second.stages[1].cached);
    const std::vector<library::StageRun> runs = unwrap(store().runs(other.id));
    ASSERT_EQ(runs.size(), 2U) << "the job's record says how each stage was done";
    EXPECT_TRUE(runs[0].cached);
    EXPECT_EQ(runs[1].findings, first.stages[1].findings);
    EXPECT_EQ(second.stages[1].findings, first.stages[1].findings) << "a result found keeps its findings";
    EXPECT_EQ(script("source")->runs, 1);
    EXPECT_EQ(script("transcript")->runs, 1) << "another job with the same inputs runs nothing again";
    EXPECT_THAT(progress(), ElementsAre("source: running", "source: done", "transcript: running",
                                        "transcript: done", "source: found done", "transcript: found done"));
}

TEST_F(ExecutorTest, AStageRunsAgainOnlyWhereWhatItReadsChanged) {
    script("transcript")->inputs = {"source"};
    const Executor stages = executor({"source", "transcript"});
    (void)run(stages, job());

    script("source")->fingerprint = json::Json{{"prompt", "v2"}};
    const RunReport same = run(stages, job());
    ASSERT_EQ(same.stages.size(), 2U);
    EXPECT_FALSE(same.stages[0].cached) << "a new fingerprint is a new key";
    EXPECT_TRUE(same.stages[1].cached) << "its artifact did not change, so what reads it is still done";

    script("source")->fingerprint = json::Json{{"prompt", "v3"}};
    script("source")->artifact = said("Goodbye.");
    const RunReport changed = run(stages, job());
    ASSERT_EQ(changed.stages.size(), 2U);
    EXPECT_FALSE(changed.stages[1].cached);
    EXPECT_EQ(script("transcript")->inputs_seen.at("source"), said("Goodbye."));
    EXPECT_EQ(script("transcript")->runs, 2);
}

TEST_F(ExecutorTest, RunIsToldWhatItsKeyWasMadeFrom) {
    script("source")->fingerprint = json::Json{{"file", "talk.vtt"}, {"content", "abc"}};
    (void)run(executor({"source"}), job());
    EXPECT_EQ(script("source")->fingerprint_seen, (json::Json{{"file", "talk.vtt"}, {"content", "abc"}}));
}

TEST_F(ExecutorTest, AFailedStageEndsTheRunAndAResumeGoesOnFromIt) {
    script("transcript")->inputs = {"source"};
    script("transcript")->error = Error{Errc::unavailable, "the pod is unreachable"};
    const Executor stages = executor({"source", "transcript"});
    const library::Job interrupted = job();

    const RunReport failed = run(stages, interrupted);
    ASSERT_TRUE(failed.error.has_value());
    EXPECT_EQ(failed.error->code(), Errc::unavailable);
    ASSERT_EQ(failed.stages.size(), 1U) << "the stages before it are reported";
    std::vector<library::StageRun> runs = unwrap(store().runs(interrupted.id));
    ASSERT_EQ(runs.size(), 2U);
    EXPECT_EQ(runs[1].stage, "transcript");
    EXPECT_EQ(runs[1].status, library::StageStatus::failed);
    EXPECT_EQ(runs[1].error, "the pod is unreachable");
    EXPECT_FALSE(runs[1].key.empty()) << "it failed once its key was known";
    EXPECT_EQ(progress().back(), "transcript: failed: the pod is unreachable");

    script("transcript")->error.reset();
    const RunReport resumed = run(stages, interrupted);
    ASSERT_FALSE(resumed.error.has_value());
    ASSERT_EQ(resumed.stages.size(), 2U);
    EXPECT_TRUE(resumed.stages[0].cached) << "the source is not read again";
    EXPECT_FALSE(resumed.stages[1].cached);
    EXPECT_EQ(script("source")->runs, 1);
    runs = unwrap(store().runs(interrupted.id));
    EXPECT_EQ(runs[1].status, library::StageStatus::done) << "the failure is replaced";
}

TEST_F(ExecutorTest, AStopEndsTheRunBeforeTheNextStage) {
    const std::stop_source stop;
    stop.request_stop();
    const library::Job stopped = job();
    const RunReport report = run(executor({"source"}), stopped, stop.get_token());
    ASSERT_TRUE(report.error.has_value());
    EXPECT_EQ(report.error->code(), Errc::cancelled);
    EXPECT_EQ(script("source")->runs, 0);
    const std::vector<library::StageRun> runs = unwrap(store().runs(stopped.id));
    ASSERT_EQ(runs.size(), 1U);
    EXPECT_EQ(runs[0].status, library::StageStatus::failed);
    EXPECT_EQ(runs[0].key, "") << "it stopped before its key was known";
}

TEST_F(ExecutorTest, AnArtifactThatBreaksItsSchemaIsNeverKept) {
    script("source")->artifact = json::Json{{"sentences", "none"}};
    const RunReport report = run(executor({"source"}), job());
    ASSERT_TRUE(report.error.has_value());
    EXPECT_EQ(report.error->code(), Errc::internal);
    EXPECT_THAT(report.error->message(), HasSubstr("breaks its schema"));
    const auto artifacts = [](db::ReadTx& tx) { return tx.query_int("SELECT count(*) FROM artifacts"); };
    EXPECT_EQ(unwrap(database().read(artifacts)), 0);
}

TEST_F(ExecutorTest, OnlyAStageThatSamplesDrawsAgainForANewSalt) {
    script("sampled")->samples = true;
    const Executor stages = executor({"fixed", "sampled"});
    (void)run(stages, job(0));
    const RunReport replicate = run(stages, job(1));
    ASSERT_EQ(replicate.stages.size(), 2U);
    EXPECT_TRUE(replicate.stages[0].cached) << "a stage that does not sample is shared by every replicate";
    EXPECT_FALSE(replicate.stages[1].cached) << "a new salt asks new samples";
    const RunReport again = run(stages, job(1));
    ASSERT_EQ(again.stages.size(), 2U);
    EXPECT_TRUE(again.stages[1].cached) << "the same salt finds them";
}

TEST_F(ExecutorTest, AResultKeptForItsJobAloneIsNotFoundByAnother) {
    script("transcript")->reusable = false;
    const Executor stages = executor({"transcript"});
    (void)run(stages, job());
    const RunReport other = run(stages, job());
    ASSERT_EQ(other.stages.size(), 1U);
    EXPECT_FALSE(other.stages[0].cached);
    EXPECT_EQ(script("transcript")->runs, 2);
}

TEST_F(ExecutorTest, StagesMustComeAfterWhatTheyRead) {
    script("transcript")->inputs = {"source"};
    const auto refused = [&](const std::vector<std::string>& names) {
        std::vector<std::unique_ptr<Stage>> stages;
        stages.reserve(names.size());
        for (const std::string& name : names) {
            stages.push_back(std::make_unique<FakeStage>(name, script(name)));
        }
        return Executor::create(std::move(stages), clock());
    };
    EXPECT_ERRC(refused({"transcript", "source"}), Errc::invalid_argument);
    EXPECT_ERRC(refused({"source", "source"}), Errc::invalid_argument);
    script("source")->schema = "artifacts/nothing";
    EXPECT_ERRC(refused({"source"}), Errc::invalid_argument)
        << "an artifact must have a schema to be checked by";
}

TEST(CacheKey, NamesEverythingTheResultDependsOn) {
    const auto made = std::make_shared<Script>();
    const FakeStage stage{"transcript", made};
    const json::Json fingerprint{{"prompt", "punctuate@1"}};
    const std::map<std::string, std::string, std::less<>> inputs{{"source", "abc"}};
    const std::string key = cache_key(stage, fingerprint, inputs, 0);
    EXPECT_EQ(key.size(), 64U) << "a BLAKE3 hash, in hex";
    EXPECT_EQ(cache_key(stage, fingerprint, inputs, 7), key)
        << "a stage that does not sample ignores the salt";
    EXPECT_NE(cache_key(stage, json::Json{{"prompt", "punctuate@2"}}, inputs, 0), key);
    EXPECT_NE(cache_key(stage, fingerprint, {{"source", "abd"}}, 0), key);
    EXPECT_NE(cache_key(FakeStage{"atomize", made}, fingerprint, inputs, 0), key);
    made->schema = "artifacts/source";
    EXPECT_NE(cache_key(stage, fingerprint, inputs, 0), key) << "the schema it writes by is part of its key";
    made->schema = "artifacts/transcript";
    made->version = 2;
    EXPECT_NE(cache_key(stage, fingerprint, inputs, 0), key) << "a new version makes something else";
    made->version = 1;
    made->samples = true;
    EXPECT_NE(cache_key(stage, fingerprint, inputs, 7), cache_key(stage, fingerprint, inputs, 0));
}

TEST(JobTier, IsAShippedTierOrAVariantUnlessTheContextFindsItsOwn) {
    const library::Job job{.id = "J1", .tier = "thorough"};
    EXPECT_EQ(unwrap(job_tier(StageContext{.job = &job})), unwrap(reason::builtin_tier("thorough")));
    const library::Job variant{.id = "J3", .tier = "fast+atomize.recall_pass=true"};
    EXPECT_TRUE(unwrap(job_tier(StageContext{.job = &variant})).atomize.recall_pass)
        << "a variant, found by its name alone: nothing is stored";
    const library::Job unknown{.id = "J2", .tier = "turbo"};
    EXPECT_ERRC(job_tier(StageContext{.job = &unknown}), Errc::not_found);

    const FindTier variants = [](std::string_view name) -> Result<reason::Tier> {
        RR_TRY_ASSIGN(reason::Tier tier, reason::builtin_tier("fast"));
        tier.name = std::string{name};
        tier.atomize.recall_pass = true;
        return tier;
    };
    const reason::Tier found = unwrap(job_tier(StageContext{.job = &unknown, .tiers = variants}));
    EXPECT_EQ(found.name, "turbo");
    EXPECT_TRUE(found.atomize.recall_pass) << "the context's own";
}

TEST(StatusAfter, AStopOrAPassingFailureCanBeResumed) {
    EXPECT_EQ(Executor::status_after(Error{Errc::cancelled, ""}), library::JobStatus::stopped);
    EXPECT_EQ(Executor::status_after(Error{Errc::budget_exceeded, ""}), library::JobStatus::stopped);
    EXPECT_EQ(Executor::status_after(Error{Errc::unavailable, ""}), library::JobStatus::stopped);
    EXPECT_EQ(Executor::status_after(Error{Errc::network, ""}), library::JobStatus::stopped);
    EXPECT_EQ(Executor::status_after(Error{Errc::needs_user, ""}), library::JobStatus::needs_user);
    EXPECT_EQ(Executor::status_after(Error{Errc::not_found, ""}), library::JobStatus::failed);
    EXPECT_EQ(Executor::status_after(Error{Errc::internal, ""}), library::JobStatus::failed);
}

// --- The army's part of a key ------------------------------------------------------------------

class Silent final : public llm::LlmProvider {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "silent"; }
    [[nodiscard]] llm::ProviderCaps caps() const noexcept override {
        return {.json_schema = true, .thinking = true};
    }
    [[nodiscard]] Result<llm::ChatResponse> chat(const llm::ChatRequest& /*request*/,
                                                 const llm::StreamSink& /*sink*/,
                                                 const std::stop_token& /*stop*/) const override {
        return fail(Errc::internal, "not called");
    }
};

agents::Engine engine(std::string model, std::string role, std::string checksum, int slots) {
    return agents::Engine{.model = std::move(model),
                          .checksum = std::move(checksum),
                          .roles = {std::move(role)},
                          .family = "qwen",
                          .slots = slots,
                          .context = 8192 * slots,
                          .ctx_per_slot = 8192,
                          .direct = llm::SamplingPreset{.temperature = 0.7},
                          .thinking = llm::SamplingPreset{
                              .thinking = true, .thinking_budget = llm::ThinkingBudget{.tokens = 100}}};
}

json::Json fingerprint_of(std::vector<agents::Engine> engines) {
    static const Silent provider;
    static const FakeClock clock;
    const agents::Army army =
        unwrap(agents::Army::create(agents::Army::Options{.engines = std::move(engines)}, provider, clock));
    return army_fingerprint(army);
}

TEST(ArmyFingerprint, NamesTheModelsAndHowTheySampleNotTheirSize) {
    const json::Json two =
        fingerprint_of({engine("qwen-b", "infantry", "sha-b", 2), engine("qwen-a", "commander", "sha-a", 1)});
    ASSERT_EQ(two.size(), 2U);
    EXPECT_EQ(two[0].at("model"), "qwen-a") << "in the order of their models, not of the pod's report";
    EXPECT_EQ(two[0].at("checksum"), "sha-a");
    EXPECT_EQ(two[0].at("roles"), json::Json::array({"commander"}));
    EXPECT_EQ(two[0].at("direct").at("temperature"), 0.7);
    EXPECT_EQ(
        fingerprint_of({engine("qwen-a", "commander", "sha-a", 4), engine("qwen-b", "infantry", "sha-b", 8)}),
        two)
        << "more slots answer faster, not otherwise";
    EXPECT_NE(fingerprint_of(
                  {engine("qwen-a", "commander", "sha-a2", 1), engine("qwen-b", "infantry", "sha-b", 2)}),
              two)
        << "another file behind the same id answers otherwise";
}

}  // namespace
}  // namespace rr::pipeline
