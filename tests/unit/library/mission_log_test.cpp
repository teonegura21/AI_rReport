#include <rr/library/mission_log.hpp>

#include <chrono>
#include <vector>

#include <gtest/gtest.h>

#include <rr/library/library.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::library {
namespace {

using namespace std::chrono_literals;
using testkit::unwrap;

const std::chrono::system_clock::time_point kStart{std::chrono::milliseconds{1'767'225'600'000}};

// A mission the commander rescued, with everything a record holds.
agents::MissionRecord rescued() {
    return agents::MissionRecord{
        .job_id = "J1",
        .mission = "atomize/w07",
        .kind = "atomize",
        .prompt = "atomize@3",
        .role = "infantry",
        .started_at = kStart,
        .duration = 4200ms,
        .verdict = agents::Verdict::accepted,
        .step = agents::Step::commander,
        .rungs = 3,
        .answered_by = "qwen3.8-27b-q4",
        .samples = 3,
        .accepted = 1,
        .calls = 7,
        .usage = {.prompt_tokens = 9000, .completion_tokens = 2100, .cached_prompt_tokens = 6000},
        .problems = {"/atoms/3/kind (enum): \"quip\" is not one of the kinds"}};
}

class MissionLogTest : public ::testing::Test {
protected:
    [[nodiscard]] db::Database& database() { return database_; }

private:
    testkit::TempDir dir_;
    db::Database database_ = unwrap(open_database(dir_ / "library.db"));
};

TEST_F(MissionLogTest, KeepsAMissionWhole) {
    MissionLog log{database()};
    ASSERT_OK(log.record(rescued()));

    const std::vector<agents::MissionRecord> missions = unwrap(log.list("J1"));
    ASSERT_EQ(missions.size(), 1U);
    const agents::MissionRecord& kept = missions.front();
    const agents::MissionRecord expected = rescued();
    EXPECT_EQ(kept.job_id, expected.job_id);
    EXPECT_EQ(kept.mission, expected.mission);
    EXPECT_EQ(kept.kind, expected.kind);
    EXPECT_EQ(kept.prompt, expected.prompt);
    EXPECT_EQ(kept.role, expected.role);
    EXPECT_EQ(kept.started_at, expected.started_at);
    EXPECT_EQ(kept.duration, expected.duration);
    EXPECT_EQ(kept.verdict, expected.verdict);
    EXPECT_FALSE(kept.error.has_value());
    EXPECT_EQ(kept.step, expected.step);
    EXPECT_EQ(kept.rungs, expected.rungs);
    EXPECT_EQ(kept.answered_by, expected.answered_by);
    EXPECT_EQ(kept.samples, expected.samples);
    EXPECT_EQ(kept.accepted, expected.accepted);
    EXPECT_EQ(kept.calls, expected.calls);
    EXPECT_EQ(kept.usage, expected.usage);
    EXPECT_EQ(kept.problems, expected.problems);
}

TEST_F(MissionLogTest, KeepsAFailureWithItsError) {
    MissionLog log{database()};
    agents::MissionRecord stopped = rescued();
    stopped.verdict.reset();
    stopped.error = Error{Errc::cancelled, "the job was stopped"};
    stopped.answered_by.clear();
    stopped.accepted = 0;
    stopped.prompt.clear();
    ASSERT_OK(log.record(stopped));

    const agents::MissionRecord kept = unwrap(log.list("J1")).at(0);
    EXPECT_FALSE(kept.verdict.has_value());
    ASSERT_TRUE(kept.error.has_value());
    EXPECT_EQ(kept.error->code(), Errc::cancelled);
    EXPECT_NE(kept.error->message().find("the job was stopped"), std::string::npos);
    EXPECT_EQ(kept.answered_by, "") << "none is stored as NULL, and read back as none";
    EXPECT_EQ(kept.prompt, "");
}

TEST_F(MissionLogTest, AMissionRunAgainReplacesItsRow) {
    MissionLog log{database()};
    agents::MissionRecord first = rescued();
    first.verdict.reset();
    first.error = Error{Errc::unavailable, "the pod stopped"};
    ASSERT_OK(log.record(first));
    ASSERT_OK(log.record(rescued()));  // the resumed job's run

    const std::vector<agents::MissionRecord> missions = unwrap(log.list("J1"));
    ASSERT_EQ(missions.size(), 1U);
    EXPECT_EQ(missions.front().verdict, agents::Verdict::accepted);
    EXPECT_FALSE(missions.front().error.has_value());
}

TEST_F(MissionLogTest, ListsAJobsMissionsInTheOrderTheyStarted) {
    MissionLog log{database()};
    agents::MissionRecord later = rescued();
    later.mission = "a-later-one";
    later.started_at = kStart + 1s;
    agents::MissionRecord other_job = rescued();
    other_job.job_id = "J2";
    agents::MissionRecord at_a_ceiling = rescued();
    at_a_ceiling.mission = "write/s01";
    at_a_ceiling.verdict = agents::Verdict::ceiling;
    at_a_ceiling.step.reset();
    at_a_ceiling.rungs = 0;
    at_a_ceiling.calls = 0;
    ASSERT_OK(log.record(later));
    ASSERT_OK(log.record(rescued()));
    ASSERT_OK(log.record(other_job));
    ASSERT_OK(log.record(at_a_ceiling));

    const std::vector<agents::MissionRecord> missions = unwrap(log.list("J1"));
    ASSERT_EQ(missions.size(), 3U);
    EXPECT_EQ(missions[0].mission, "atomize/w07") << "same start: by name";
    EXPECT_EQ(missions[1].mission, "write/s01");
    EXPECT_FALSE(missions[1].step.has_value()) << "no rung was climbed";
    EXPECT_EQ(missions[2].mission, "a-later-one");
    EXPECT_TRUE(unwrap(log.list("J3")).empty());
}

TEST_F(MissionLogTest, TheTableRefusesARecordThatContradictsItself) {
    MissionLog log{database()};
    agents::MissionRecord neither = rescued();
    neither.verdict.reset();  // neither a verdict nor an error
    EXPECT_FALSE(log.record(neither).has_value());

    agents::MissionRecord too_many = rescued();
    too_many.accepted = 4;  // of three samples
    EXPECT_FALSE(log.record(too_many).has_value());

    agents::MissionRecord no_step = rescued();
    no_step.step.reset();  // yet three rungs
    EXPECT_FALSE(log.record(no_step).has_value());
}

}  // namespace
}  // namespace rr::library
