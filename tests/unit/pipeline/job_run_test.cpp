#include <rr/pipeline/job_run.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <rr/library/call_memo.hpp>
#include <rr/library/library.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

#include "fake_stage.hpp"

namespace rr::pipeline {
namespace {

using fakes::FakeStage;
using fakes::Script;
using namespace std::chrono_literals;
using testkit::unwrap;

class JobRunTest : public ::testing::Test {
protected:
    // The source stage on a caption file, then a stage the test scripts,
    // which takes two seconds.
    JobRunTest() {
        script_->inputs = {"source"};
        script_->on_run = [this](const StageContext&) { clock_.advance(2s); };
        std::vector<std::unique_ptr<Stage>> stages;
        stages.push_back(std::make_unique<SourceStage>());
        stages.push_back(std::make_unique<FakeStage>("transcript", script_));
        executor_.emplace(unwrap(Executor::create(std::move(stages), clock_)));
    }

    library::Job job() {
        const auto file =
            dir_.write_file("On radium.vtt", "WEBVTT\n\n00:00.000 --> 00:02.000\nRadium glows.\n");
        return unwrap(jobs_.create(file.string(), "max", 0));
    }

    JobRun run(library::Job job) {
        return unwrap(run_job(std::move(job), *executor_, database_, clock_, StageContext{}));
    }

    [[nodiscard]] testkit::TempDir& dir() { return dir_; }
    [[nodiscard]] db::Database& database() { return database_; }
    [[nodiscard]] FakeClock& clock() { return clock_; }
    [[nodiscard]] library::Jobs& jobs() { return jobs_; }
    [[nodiscard]] std::shared_ptr<Script>& script() { return script_; }

private:
    testkit::TempDir dir_;
    db::Database database_ = unwrap(library::open_database(dir_ / "library.db"));
    FakeClock clock_;
    library::Jobs jobs_{database_, clock_};
    std::shared_ptr<Script> script_ = std::make_shared<Script>();
    std::optional<Executor> executor_;
};

TEST_F(JobRunTest, ADoneJobHasItsTitleAndForgetsItsModelAnswers) {
    const library::Job started = job();
    library::CallMemo memo{database(), started.id, clock()};
    ASSERT_OK(memo.keep("K1", llm::ChatResponse{.model = "gemma"}));

    const JobRun done = run(started);
    EXPECT_FALSE(done.report.error.has_value());
    EXPECT_EQ(done.job.status, library::JobStatus::done);
    EXPECT_EQ(done.job.title, "On radium") << "as its source names it";
    EXPECT_EQ(done.job.elapsed, std::chrono::milliseconds{2000});
    EXPECT_EQ(unwrap(jobs().find(started.id)), done.job) << "as saved";
    EXPECT_EQ(unwrap(memo.find("K1")), std::nullopt) << "its artifacts are the truth now (ADR-0020)";
}

TEST_F(JobRunTest, ItsVideosLengthIsKnownOnceItsSourceIsRead) {
    const library::Job started = job();
    const library::StageStore store{database(), clock()};
    EXPECT_ERRC(video_length(store, started.id), Errc::not_found) << "before its source stage";
    std::optional<Result<std::chrono::milliseconds>> during;  // as the army raised by a later stage asks it
    script()->on_run = [&](const StageContext&) { during = video_length(store, started.id); };
    run(started);
    ASSERT_TRUE(during.has_value());
    EXPECT_EQ(unwrap(*during), std::chrono::milliseconds{2'000}) << "where its captions end";
}

TEST_F(JobRunTest, AJobIsSavedRunningBeforeItsStagesStart) {
    const library::Job started = job();
    std::optional<library::JobStatus> seen;
    script()->on_run = [&](const StageContext& context) {
        seen = unwrap(jobs().find(context.job->id)).status;
    };
    library::Job stopped = started;
    stopped.status = library::JobStatus::stopped;
    stopped.error = "the pod is unreachable";
    stopped = unwrap(jobs().save(stopped));  // as a run that stopped left it
    (void)run(std::move(stopped));
    EXPECT_EQ(seen, library::JobStatus::running) << "a crash leaves it running, for resume to take up";
}

TEST_F(JobRunTest, AStoppedJobSaysWhyAndAResumeFinishesIt) {
    const library::Job started = job();
    script()->error = Error{Errc::unavailable, "the pod is unreachable"};
    const JobRun stopped = run(started);
    EXPECT_EQ(stopped.job.status, library::JobStatus::stopped) << "a pod that comes back lets it go on";
    EXPECT_EQ(stopped.job.error, "the pod is unreachable");
    EXPECT_EQ(stopped.job.title, "On radium") << "its source was read";
    EXPECT_EQ(stopped.job.elapsed, std::chrono::milliseconds{2000});

    script()->error.reset();
    const JobRun resumed = run(stopped.job);
    EXPECT_EQ(resumed.job.status, library::JobStatus::done);
    EXPECT_EQ(resumed.job.error, std::nullopt);
    ASSERT_EQ(resumed.report.stages.size(), 2U);
    EXPECT_TRUE(resumed.report.stages[0].cached) << "the source is not read again";
    EXPECT_EQ(resumed.job.elapsed, std::chrono::milliseconds{4000}) << "its time, across runs";
}

TEST_F(JobRunTest, AJobWhoseSourceIsGoneFails) {
    const library::Job started = unwrap(jobs().create((dir() / "missing.vtt").string(), "max", 0));
    const JobRun failed = run(started);
    EXPECT_EQ(failed.job.status, library::JobStatus::failed);
    EXPECT_TRUE(failed.job.error.has_value());
    EXPECT_EQ(failed.job.title, std::nullopt);
}

}  // namespace
}  // namespace rr::pipeline
