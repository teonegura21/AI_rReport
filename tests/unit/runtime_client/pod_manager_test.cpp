// PodManager with a fake RunPod API and a real runtime (a Supervisor with
// fake engines) standing in for what runs inside the pod.
#include <rr/runtime_client/pod_manager.hpp>

#include <chrono>
#include <string>
#include <thread>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/fakes/fake_runpod.hpp>
#include <rr/testkit/result.hpp>

#include "test_runtime.hpp"

namespace rr::runtime_client {
namespace {

using namespace std::chrono_literals;
using fakes::FakeRunpod;
using runpod::PodStatus;
using ::testing::HasSubstr;
using testkit::unwrap;
using Clock = std::chrono::steady_clock;

// A pod "abc123": RunPod's side faked, the runtime inside it real.
class PodManagerTest : public ::testing::Test {
protected:
    [[nodiscard]] FakeRunpod& runpod() { return runpod_; }
    [[nodiscard]] const PodManager& manager() const { return manager_; }

private:
    FakeRunpod runpod_;
    testing::Runtime runtime_{R"("--fake-load-ms", "300")"};
    net::HttpClient http_;
    runpod::RunpodApi api_{http_,
                           runpod::Options{.api_key = Secret{std::string{FakeRunpod::kAccountKey}},
                                           .api_url = runpod_.api_url(),
                                           .graphql_url = runpod_.graphql_url()},
                           net::RetryPolicy::none()};
    RuntimeClient client_{http_, runtime_.endpoint(), net::RetryPolicy::none()};
    PodManager manager_{api_, "abc123", client_};
};

TEST_F(PodManagerTest, StartsAStoppedPodAndWaitsForItsEngines) {
    runpod().add_pod("abc123", "EXITED");
    runpod().set_boot_reads(3);
    const ReadyPod ready = unwrap(manager().ensure_ready(20s, {}, 20ms));
    EXPECT_TRUE(ready.started);
    EXPECT_EQ(ready.pod.status, PodStatus::running);
    EXPECT_EQ(ready.health.engines.size(), 2U);
    EXPECT_GT(ready.waited, 0ms) << "the cold start, measured";
    EXPECT_EQ(runpod().actions("start"), 1);
}

TEST_F(PodManagerTest, APodStillReportedStoppedAfterItsStartIsWaitedFor) {
    runpod().add_pod("abc123", "EXITED");
    runpod().set_start_lag(3);  // RunPod's reads lag its writes: EXITED three more times
    const ReadyPod ready = unwrap(manager().ensure_ready(20s, {}, 20ms));
    EXPECT_TRUE(ready.started);
    EXPECT_EQ(ready.pod.status, PodStatus::running);
    EXPECT_EQ(runpod().actions("start"), 1) << "started once, then waited for";
}

TEST_F(PodManagerTest, APodSomeoneElseStartedMeanwhileIsWaitedFor) {
    runpod().add_pod("abc123", "STARTING");
    runpod().serve_stale("abc123", "EXITED", 1);  // read as stopped, then started by another hub
    const ReadyPod ready = unwrap(manager().ensure_ready(20s, {}, 20ms));
    EXPECT_EQ(ready.pod.status, PodStatus::running);
    EXPECT_EQ(runpod().actions("start"), 1) << "asked for, and refused: it was already starting";
}

TEST_F(PodManagerTest, ARunningPodIsOnlyWaitedFor) {
    runpod().add_pod("abc123", "RUNNING");
    const ReadyPod ready = unwrap(manager().ensure_ready(20s, {}, 20ms));
    EXPECT_FALSE(ready.started);
    EXPECT_EQ(runpod().actions("start"), 0);
}

TEST_F(PodManagerTest, AFullMachineIsReportedAtOnce) {
    runpod().add_pod("abc123", "EXITED");
    runpod().set_no_free_gpus(true);
    const auto started = Clock::now();
    const auto ready = manager().ensure_ready(20s, {}, 20ms);
    ASSERT_ERRC(ready, Errc::unavailable);
    EXPECT_THAT(ready.error().message(), HasSubstr("not enough free GPUs"));
    EXPECT_LT(Clock::now() - started, 5s) << "not retried for as long as the timeout allows";
}

TEST_F(PodManagerTest, APodThatFallsBackEndsTheWait) {
    runpod().add_pod("abc123", "EXITED");
    runpod().set_boot_reads(1'000'000);  // never finishes booting on its own
    const std::jthread crash{[this] {
        std::this_thread::sleep_for(200ms);
        runpod().add_pod("abc123", "ERROR");
    }};
    const auto ready = manager().ensure_ready(20s, {}, 20ms);
    ASSERT_ERRC(ready, Errc::unavailable);
    EXPECT_THAT(ready.error().message(), HasSubstr("is ERROR instead of starting"));

    runpod().add_pod("abc123", "TERMINATED");
    EXPECT_ERRC(manager().ensure_ready(20s, {}, 20ms), Errc::not_found)
        << "a terminated pod cannot come back";
}

TEST_F(PodManagerTest, GivesUpOnAPodThatNeverRuns) {
    runpod().add_pod("abc123", "PROVISIONING");
    const auto ready = manager().ensure_ready(300ms, {}, 20ms);
    ASSERT_ERRC(ready, Errc::timeout);
    EXPECT_THAT(ready.error().message(), HasSubstr("still PROVISIONING"));
}

TEST_F(PodManagerTest, StopsAPodThatIsUpAndLeavesAStoppedOneAlone) {
    runpod().add_pod("abc123", "RUNNING");
    EXPECT_EQ(unwrap(manager().stop_pod()).status, PodStatus::exited);
    EXPECT_EQ(unwrap(manager().stop_pod()).status, PodStatus::exited);
    EXPECT_EQ(runpod().actions("stop"), 1) << "the second stop found it stopped";
    EXPECT_EQ(unwrap(manager().status()).status, PodStatus::exited);
}

}  // namespace
}  // namespace rr::runtime_client
