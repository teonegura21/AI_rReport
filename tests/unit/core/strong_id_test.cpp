#include <rr/core/strong_id.hpp>

#include <format>
#include <type_traits>
#include <unordered_set>

#include <gtest/gtest.h>

namespace rr {
namespace {

struct JobTag {};

struct VideoTag {};

using JobId = StrongId<JobTag>;
using VideoId = StrongId<VideoTag>;

// The whole point: different ID kinds do not convert into each other, and a
// raw string does not silently become an ID.
static_assert(!std::is_convertible_v<JobId, VideoId>);
static_assert(!std::is_convertible_v<std::string, JobId>);
static_assert(std::is_constructible_v<JobId, std::string>);

TEST(StrongId, ComparesByValue) {
    EXPECT_EQ(JobId{"a"}, JobId{"a"});
    EXPECT_NE(JobId{"a"}, JobId{"b"});
    EXPECT_LT(JobId{"a"}, JobId{"b"});
}

TEST(StrongId, WorksAsHashKeyAndFormats) {
    const std::unordered_set<JobId> ids{JobId{"x"}, JobId{"y"}, JobId{"x"}};
    EXPECT_EQ(ids.size(), 2U);
    EXPECT_EQ(std::format("job={}", JobId{"01J9"}), "job=01J9");
}

}  // namespace
}  // namespace rr
