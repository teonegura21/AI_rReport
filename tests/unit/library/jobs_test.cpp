#include <rr/library/jobs.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <rr/library/library.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::library {
namespace {

using namespace std::chrono_literals;
using testkit::unwrap;

class JobsTest : public ::testing::Test {
protected:
    [[nodiscard]] FakeClock& clock() { return clock_; }
    [[nodiscard]] Jobs& jobs() { return jobs_; }

private:
    testkit::TempDir dir_;
    db::Database database_ = unwrap(open_database(dir_ / "library.db"));
    FakeClock clock_;
    Jobs jobs_{database_, clock_};
};

TEST_F(JobsTest, ACreatedJobIsRunningAndFoundAgain) {
    const Job job = unwrap(jobs().create("https://www.youtube.com/watch?v=x", "max", 0));
    EXPECT_EQ(job.id.size(), 26U) << "a ULID";
    EXPECT_EQ(job.status, JobStatus::running);
    EXPECT_EQ(job.source, "https://www.youtube.com/watch?v=x");
    EXPECT_EQ(job.tier, "max");
    EXPECT_EQ(job.created_at, clock().now());
    EXPECT_EQ(unwrap(jobs().find(job.id)), job);
}

TEST_F(JobsTest, JobsAreListedNewestFirst) {
    std::vector<std::string> ids;
    ids.reserve(3);
    for (int index = 0; index < 3; ++index) {
        ids.push_back(unwrap(jobs().create("talk.vtt", "fast", index)).id);
    }
    const std::vector<Job> recent = unwrap(jobs().recent(2));
    ASSERT_EQ(recent.size(), 2U);
    EXPECT_EQ(recent[0].id, ids[2]) << "jobs made in one millisecond still keep their order";
    EXPECT_EQ(recent[1].id, ids[1]);
}

TEST_F(JobsTest, SavingKeepsWhereAJobStands) {
    Job job = unwrap(jobs().create("talk.vtt", "max", 0));
    clock().advance(5s);
    job.status = JobStatus::needs_user;
    job.error = "yt-dlp is not installed";
    job.title = "On radium";
    job.elapsed = std::chrono::milliseconds{4200};
    const Job saved = unwrap(jobs().save(job));
    EXPECT_EQ(saved.updated_at, clock().now());
    EXPECT_EQ(unwrap(jobs().find(job.id)), saved);
}

TEST_F(JobsTest, AJobNeverCreatedIsNotFound) {
    EXPECT_EQ(jobs().find("01J00000000000000000000000").error().code(), Errc::not_found);
    EXPECT_EQ(jobs().save(Job{.id = "nope"}).error().code(), Errc::not_found);
}

TEST(JobStatus, NamesItsStatuses) {
    for (const JobStatus status : {JobStatus::running, JobStatus::done, JobStatus::failed,
                                   JobStatus::needs_user, JobStatus::stopped}) {
        EXPECT_EQ(job_status_from_string(to_string(status)), status);
    }
    EXPECT_EQ(to_string(JobStatus::needs_user), "needs_user");
    EXPECT_EQ(job_status_from_string("paused"), std::nullopt);
}

}  // namespace
}  // namespace rr::library
