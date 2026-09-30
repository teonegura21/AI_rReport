#include <rr/supervisor/gpu.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

namespace rr::supervisor {
namespace {

using ::testing::HasSubstr;
using testkit::unwrap;

TEST(NvidiaSmi, ParsesOneLinePerGpu) {
    const auto gpus =
        unwrap(parse_nvidia_smi("0, NVIDIA L40S, 46068, 41012, 87, 570.124.06\n"
                                "1, NVIDIA GeForce RTX 4090, 24564, 3, 0, 570.124.06\n"));
    ASSERT_EQ(gpus.size(), 2U);
    EXPECT_EQ(gpus[0], (runtime_api::GpuInfo{.index = 0,
                                             .name = "NVIDIA L40S",
                                             .memory_total_mib = 46068,
                                             .memory_used_mib = 41012,
                                             .utilization_percent = 87,
                                             .driver_version = "570.124.06"}));
    EXPECT_EQ(gpus[1].name, "NVIDIA GeForce RTX 4090");
    EXPECT_EQ(gpus[1].memory_total_mib, 24564);
}

TEST(NvidiaSmi, ReadsUnavailableFieldsAsZero) {
    const auto gpus =
        unwrap(parse_nvidia_smi("0, NVIDIA A100 MIG 3g.40gb, 40192, [N/A], [Not Supported], 550.54\r\n"));
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_EQ(gpus[0].memory_used_mib, 0);
    EXPECT_EQ(gpus[0].utilization_percent, 0);
    EXPECT_TRUE(unwrap(parse_nvidia_smi("\n  \n")).empty()) << "no GPU lines, no GPUs";
}

TEST(NvidiaSmi, RejectsMalformedOutput) {
    EXPECT_THAT(parse_nvidia_smi("0, L40S, 46068\n").error().message(), HasSubstr("expected 6 fields"));
    EXPECT_THAT(parse_nvidia_smi("0, L40S, lots, 1, 2, 570\n").error().message(),
                HasSubstr("memory.total is 'lots'"));
}

TEST(NvidiaSmi, AMissingToolMeansNoGpu) {
    EXPECT_ERRC(query_gpus("rr-no-such-nvidia-smi"), Errc::unavailable);
}

}  // namespace
}  // namespace rr::supervisor
