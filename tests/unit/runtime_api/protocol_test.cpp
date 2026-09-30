#include <rr/runtime_api/protocol.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

namespace rr::runtime_api {
namespace {

using ::testing::HasSubstr;
using testkit::unwrap;

HealthReport sample_report() {
    return HealthReport{
        .version = "0.1.0",
        .profile = "48gb",
        .gpus = {GpuInfo{.index = 0,
                         .name = "NVIDIA L40S",
                         .memory_total_mib = 46068,
                         .memory_used_mib = 41000,
                         .utilization_percent = 87,
                         .driver_version = "570.124.06"}},
        .engines = {EngineInfo{
                        .model = "qwen3.8-27b", .role = "commander", .state = EngineState::ready, .slots = 4},
                    EngineInfo{.model = "gemma-4-26b-a4b",
                               .role = "infantry",
                               .state = EngineState::failed,
                               .slots = 16,
                               .restarts = 2,
                               .last_error = "exit code 1"}},
        .uptime_s = 3600,
        .idle_s = 12,
        .stops_in_s = 1188};
}

TEST(HealthReport, RoundTripsThroughJson) {
    const HealthReport report = sample_report();
    EXPECT_EQ(unwrap(health_report_from_json(to_json(report))), report);
    HealthReport without_optionals = report;
    without_optionals.stops_in_s.reset();
    without_optionals.engines[1].last_error.reset();
    EXPECT_EQ(unwrap(health_report_from_json(to_json(without_optionals))), without_optionals);
}

TEST(HealthReport, ParsingIsStrictAndNamesThePath) {
    json::Json document = to_json(sample_report());
    document["gpus"][0]["memory_total_mib"] = "46068";  // a string, not a number
    const auto wrong_type = health_report_from_json(document);
    ASSERT_ERRC(wrong_type, Errc::invalid_argument);
    EXPECT_THAT(wrong_type.error().message(), HasSubstr("'gpus[0].memory_total_mib' must be an integer"));

    document = to_json(sample_report());
    document["engines"][1].erase("state");
    EXPECT_THAT(health_report_from_json(document).error().message(),
                HasSubstr("'engines[1].state' is missing"));

    document = to_json(sample_report());
    document["engines"][0]["state"] = "exploded";
    EXPECT_THAT(health_report_from_json(document).error().message(), HasSubstr("unknown value 'exploded'"));

    EXPECT_ERRC(health_report_from_json(json::Json::array()), Errc::invalid_argument);
}

TEST(HealthReport, RefusesAnotherProtocolVersion) {
    json::Json document = to_json(sample_report());
    document["protocol"] = "2";
    EXPECT_ERRC(health_report_from_json(document), Errc::unsupported);
}

TEST(LibraryNames, OnlySafeDirectoryNamesAreAccepted) {
    for (const std::string_view good : {"default", "my-library", "lib_2", "a"}) {
        EXPECT_OK(validate_library_name(good)) << good;
    }
    for (const std::string_view bad : {"", "..", "../etc", "a/b", "Upper", "-leading", "has space", "dot.ted",
                                       "0123456789012345678901234567890123456789012345678901234567890123x"}) {
        EXPECT_ERRC(validate_library_name(bad), Errc::invalid_argument) << bad;
    }
}

TEST(Paths, BuildBlobPaths) {
    EXPECT_EQ(blob_path("default", "ab12"), "/v1/blobs/default/ab12");
}

}  // namespace
}  // namespace rr::runtime_api
