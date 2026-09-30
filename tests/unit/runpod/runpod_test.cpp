#include <rr/runpod/runpod.hpp>

#include <chrono>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/fakes/fake_runpod.hpp>
#include <rr/testkit/result.hpp>

namespace rr::runpod {
namespace {

using namespace std::chrono_literals;
using fakes::FakeRunpod;
using ::testing::Contains;
using ::testing::HasSubstr;
using testkit::unwrap;

class RunpodTest : public ::testing::Test {
protected:
    [[nodiscard]] FakeRunpod& runpod() { return runpod_; }
    [[nodiscard]] RunpodApi api(std::string_view key = FakeRunpod::kAccountKey,
                                net::RetryPolicy retry = net::RetryPolicy::none()) const {
        return RunpodApi{http_,
                         Options{.api_key = Secret{std::string{key}},
                                 .api_url = runpod_.api_url() + "/",
                                 .graphql_url = runpod_.graphql_url()},
                         retry};
    }

private:
    FakeRunpod runpod_;
    net::HttpClient http_;
};

TEST_F(RunpodTest, ReadsAPod) {
    runpod().add_pod("abc123", "RUNNING");
    const Pod pod = unwrap(api().get_pod("abc123"));
    EXPECT_EQ(pod.id, "abc123");
    EXPECT_EQ(pod.name, "rreport-abc123");
    EXPECT_EQ(pod.status, PodStatus::running);
    EXPECT_EQ(pod.status_name, "RUNNING");
    EXPECT_THAT(pod.actions, Contains("stop"));
    EXPECT_EQ(pod.gpu, "NVIDIA GeForce RTX 4090");
    EXPECT_EQ(pod.gpu_count, 1);
    EXPECT_DOUBLE_EQ(pod.cost_per_hour, 0.44);
    EXPECT_EQ(pod.uptime_s, 42);
    EXPECT_EQ(pod.data_center, "EU-RO-1");
}

TEST_F(RunpodTest, StartsAndStopsAPod) {
    runpod().add_pod("abc123");
    const RunpodApi runpod_api = api();
    EXPECT_EQ(unwrap(runpod_api.get_pod("abc123")).status, PodStatus::exited);
    EXPECT_FALSE(unwrap(runpod_api.get_pod("abc123")).uptime_s.has_value()) << "no runtime while stopped";

    EXPECT_EQ(unwrap(runpod_api.start_pod("abc123")).status, PodStatus::starting);
    EXPECT_EQ(unwrap(runpod_api.get_pod("abc123")).status, PodStatus::running) << "it booted";
    EXPECT_EQ(unwrap(runpod_api.stop_pod("abc123")).status, PodStatus::exited);
    EXPECT_EQ(runpod().actions("start"), 1);
    EXPECT_EQ(runpod().actions("stop"), 1);
}

TEST_F(RunpodTest, RefusalsKeepRunpodsExplanation) {
    runpod().add_pod("abc123", "RUNNING");
    const auto again = api().start_pod("abc123");
    ASSERT_ERRC(again, Errc::conflict);
    EXPECT_THAT(again.error().message(), HasSubstr("cannot start a pod that is RUNNING"));

    const auto missing = api().get_pod("nosuchpod");
    ASSERT_ERRC(missing, Errc::not_found);
    EXPECT_THAT(missing.error().message(), HasSubstr("The requested Pod does not exist."));

    EXPECT_ERRC(api("wrong-key").get_pod("abc123"), Errc::permission_denied);
    EXPECT_ERRC(api().get_pod("../pods"), Errc::invalid_argument) << "refused before any request";
    EXPECT_ERRC(api().get_pod(""), Errc::invalid_argument);
}

TEST_F(RunpodTest, AFullMachineIsExplained) {
    runpod().add_pod("abc123");
    runpod().set_no_free_gpus(true);
    const auto started = api().start_pod("abc123");
    ASSERT_ERRC(started, Errc::unavailable);
    EXPECT_THAT(started.error().message(), HasSubstr("not enough free GPUs"));
    EXPECT_THAT(started.error().message(), HasSubstr("resumes on its own machine only"));
    EXPECT_THAT(started.error().message(), HasSubstr("same network volume"));
}

TEST_F(RunpodTest, ThrottlingCarriesTheServersDelay) {
    runpod().add_pod("abc123", "RUNNING");
    runpod().throttle_next(1);
    const auto throttled = api().get_pod("abc123");
    ASSERT_ERRC(throttled, Errc::rate_limited);
    EXPECT_EQ(throttled.error().retry_after(), 1000ms);

    runpod().throttle_next(1);
    EXPECT_EQ(
        unwrap(api(FakeRunpod::kAccountKey, net::RetryPolicy{.max_attempts = 2}).get_pod("abc123")).status,
        PodStatus::running)
        << "a retry after the pause succeeds";
}

TEST_F(RunpodTest, APodStopsItselfEvenWhenRestRefusesItsKey) {
    runpod().add_pod("abc123", "RUNNING");
    EXPECT_ERRC(api(FakeRunpod::kPodKey).stop_pod("abc123"), Errc::permission_denied)
        << "as reported in 2026";
    ASSERT_OK(api(FakeRunpod::kPodKey).stop_self("abc123"));
    EXPECT_EQ(runpod().status("abc123"), "EXITED");
    EXPECT_EQ(runpod().graphql_stops(), 1) << "through GraphQL";
}

TEST_F(RunpodTest, AKeyRestAcceptsNeedsNoGraphql) {
    runpod().add_pod("abc123", "RUNNING");
    ASSERT_OK(api().stop_self("abc123"));
    EXPECT_EQ(runpod().status("abc123"), "EXITED");
    EXPECT_EQ(runpod().graphql_stops(), 0);

    EXPECT_ERRC(api().stop_self("nosuchpod"), Errc::not_found) << "only a refused key falls back";
    EXPECT_EQ(runpod().graphql_stops(), 0);
}

TEST(Runpod, NamesAndUrls) {
    EXPECT_EQ(proxy_url("abc123", 8080), "https://abc123-8080.proxy.runpod.net");
    for (const PodStatus status : {PodStatus::provisioning, PodStatus::starting, PodStatus::running,
                                   PodStatus::exited, PodStatus::error, PodStatus::terminated}) {
        EXPECT_EQ(pod_status_from_string(to_string(status)), status);
    }
    EXPECT_EQ(pod_status_from_string("MIGRATING"), PodStatus::unknown) << "a status added later";
    EXPECT_OK(validate_pod_id("7h9k2m4n6p"));
    EXPECT_ERRC(validate_pod_id(std::string(65, 'a')), Errc::invalid_argument);
    EXPECT_ERRC(validate_pod_id("abc\"}) { x"), Errc::invalid_argument)
        << "nothing that could escape a query";
}

}  // namespace
}  // namespace rr::runpod
