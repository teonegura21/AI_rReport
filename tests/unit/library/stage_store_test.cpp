#include <rr/library/stage_store.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <rr/core/hash.hpp>
#include <rr/library/jobs.hpp>
#include <rr/library/library.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::library {
namespace {

using namespace std::chrono_literals;
using testkit::unwrap;

class StageStoreTest : public ::testing::Test {
protected:
    // A transcript stage's result for `job`, under `key`.
    [[nodiscard]] StageResult result(const std::string& job, const std::string& key,
                                     json::Json artifact) const {
        return StageResult{.job_id = job,
                           .stage = "transcript",
                           .version = 1,
                           .key = key,
                           .schema = "artifacts/transcript",
                           .artifact = std::move(artifact),
                           .findings = {"S3 to S9 were not punctuated"},
                           .started_at = clock_.now()};
    }

    std::int64_t count(std::string_view table) {
        return unwrap(database_.read([&](db::ReadTx& tx) {
            return tx.query_int(std::string{"SELECT count(*) FROM "} + std::string{table});
        }));
    }

    [[nodiscard]] db::Database& database() { return database_; }
    [[nodiscard]] FakeClock& clock() { return clock_; }
    [[nodiscard]] Jobs& jobs() { return jobs_; }
    [[nodiscard]] StageStore& store() { return store_; }
    [[nodiscard]] const json::Json& sentences() const { return sentences_; }

private:
    testkit::TempDir dir_;
    db::Database database_ = unwrap(open_database(dir_ / "library.db"));
    FakeClock clock_;
    Jobs jobs_{database_, clock_};
    StageStore store_{database_, clock_};
    const json::Json sentences_ = json::Json{{"sentences", json::Json::array()}};
};

TEST_F(StageStoreTest, ACommittedStageIsCachedAndItsArtifactKept) {
    const Job job = unwrap(jobs().create("talk.vtt", "max", 0));
    const StageResult done = result(job.id, "K1", sentences());  // started now
    const auto started = clock().now();
    clock().advance(2s);
    const std::string hash = unwrap(store().commit(done));
    EXPECT_EQ(hash, json::content_hash(sentences()).hex()) << "an artifact is kept under its content's hash";
    EXPECT_EQ(unwrap(store().cached("K1")),
              (CachedResult{.artifact = hash, .findings = {"S3 to S9 were not punctuated"}}));
    EXPECT_EQ(unwrap(store().artifact(hash)), sentences());
    const std::vector<StageRun> runs = unwrap(store().runs(job.id));
    ASSERT_EQ(runs.size(), 1U);
    EXPECT_EQ(runs[0].status, StageStatus::done);
    EXPECT_FALSE(runs[0].cached);
    EXPECT_EQ(runs[0].artifact, hash);
    EXPECT_EQ(runs[0].findings, done.findings);
    EXPECT_EQ(runs[0].started_at, started);
    EXPECT_EQ(runs[0].finished_at, clock().now());
}

TEST_F(StageStoreTest, AnArtifactIsKeptOnce) {
    const Job first = unwrap(jobs().create("talk.vtt", "max", 0));
    const Job second = unwrap(jobs().create("talk.vtt", "fast", 0));
    const std::string one = unwrap(store().commit(result(first.id, "K1", sentences())));
    const std::string two = unwrap(store().commit(result(second.id, "K2", sentences())));
    EXPECT_EQ(one, two);
    EXPECT_EQ(count("artifacts"), 1);
    EXPECT_EQ(count("stage_cache"), 2);
}

TEST_F(StageStoreTest, AStageTheCacheAnsweredIsRecordedSo) {
    const Job first = unwrap(jobs().create("talk.vtt", "max", 0));
    const Job second = unwrap(jobs().create("talk.vtt", "max", 0));
    (void)unwrap(store().commit(result(first.id, "K1", sentences())));
    const CachedResult cached = *unwrap(store().cached("K1"));
    ASSERT_OK(store().reuse(second.id, "transcript", "K1", cached, clock().now()));
    const std::vector<StageRun> runs = unwrap(store().runs(second.id));
    ASSERT_EQ(runs.size(), 1U);
    EXPECT_TRUE(runs[0].cached);
    EXPECT_EQ(runs[0].artifact, cached.artifact);
    EXPECT_EQ(runs[0].findings, cached.findings) << "a job that reuses a result publishes its findings";
}

TEST_F(StageStoreTest, AResultCutShortByItsJobIsKeptForThatJobAlone) {
    const Job job = unwrap(jobs().create("talk.vtt", "max", 0));
    StageResult cut = result(job.id, "K1", sentences());
    cut.reusable = false;
    cut.findings = {"S3 to S9 were not punctuated: the job reached its completion-token ceiling"};
    const std::string hash = unwrap(store().commit(cut));
    EXPECT_EQ(unwrap(store().cached("K1")), std::nullopt) << "no other job is served it";
    EXPECT_EQ(unwrap(store().artifact(hash)), sentences()) << "its artifact is kept";
    const std::vector<StageRun> runs = unwrap(store().runs(job.id));
    ASSERT_EQ(runs.size(), 1U);
    EXPECT_EQ(runs[0].artifact, hash);
    EXPECT_EQ(runs[0].findings, cut.findings);
}

TEST_F(StageStoreTest, AFailedStageIsRecordedAndARetryReplacesIt) {
    const Job job = unwrap(jobs().create("talk.vtt", "max", 0));
    ASSERT_OK(store().record_failure(job.id, "transcript", "K1", "the pod is unreachable", clock().now()));
    std::vector<StageRun> runs = unwrap(store().runs(job.id));
    ASSERT_EQ(runs.size(), 1U);
    EXPECT_EQ(runs[0].status, StageStatus::failed);
    EXPECT_EQ(runs[0].error, "the pod is unreachable");
    EXPECT_EQ(runs[0].artifact, std::nullopt);
    EXPECT_TRUE(runs[0].findings.empty());
    (void)unwrap(store().commit(result(job.id, "K1", sentences())));
    runs = unwrap(store().runs(job.id));
    ASSERT_EQ(runs.size(), 1U);
    EXPECT_EQ(runs[0].status, StageStatus::done);
    EXPECT_EQ(runs[0].error, std::nullopt);
}

TEST_F(StageStoreTest, ACommitIsAllOrNothing) {
    const Result<std::string> commit = store().commit(result("a job never created", "K1", sentences()));
    ASSERT_FALSE(commit.has_value()) << "its run names no job";
    EXPECT_EQ(unwrap(store().cached("K1")), std::nullopt) << "so its cache entry is not kept";
    EXPECT_EQ(count("artifacts"), 0) << "nor its artifact";
}

TEST_F(StageStoreTest, ADamagedArtifactIsRefused) {
    const Job job = unwrap(jobs().create("talk.vtt", "max", 0));
    const std::string hash = unwrap(store().commit(result(job.id, "K1", sentences())));
    ASSERT_OK(database().write([&](db::WriteTx& tx) -> Result<void> {
        RR_TRY(tx.execute("UPDATE artifacts SET body = CAST('{\"sentences\":[1]}' AS BLOB) WHERE hash = ?1",
                          {hash}));
        return ok();
    }));
    EXPECT_EQ(store().artifact(hash).error().code(), Errc::integrity);
}

TEST_F(StageStoreTest, WhatWasNeverKeptIsNotFound) {
    EXPECT_EQ(unwrap(store().cached("nothing")), std::nullopt);
    EXPECT_EQ(store().artifact(blake3("nothing").hex()).error().code(), Errc::not_found);
    EXPECT_TRUE(unwrap(store().runs("nobody")).empty());
}

}  // namespace
}  // namespace rr::library
