#include <rr/db/value.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include <gtest/gtest.h>

namespace rr::db {
namespace {

using VideoId = StrongId<struct VideoIdTag>;

// Which C++ types bind as which SQLite storage class is part of the API, so
// the rules are pinned down at compile time as well as at run time.
static_assert(std::is_convertible_v<int, Value>);
static_assert(std::is_convertible_v<std::uint32_t, Value>);
static_assert(std::is_convertible_v<std::size_t, Value>);
static_assert(std::is_convertible_v<float, Value>);
static_assert(std::is_convertible_v<const char*, Value>);
static_assert(std::is_convertible_v<std::string_view, Value>);
static_assert(std::is_convertible_v<std::optional<std::int64_t>, Value>);
static_assert(std::is_convertible_v<VideoId, Value>);
static_assert(!std::is_convertible_v<char, Value>, "'x' is a character, not the number 120");
static_assert(!std::is_convertible_v<std::chrono::milliseconds, Value>, "durations must name their unit");
static_assert(!std::is_convertible_v<void*, Value>, "pointers must not decay to booleans");

template <class T>
const T& held(const Value& value) {
    return std::get<T>(value.storage());
}

TEST(Value, IntegersAndBooleansBecomeInt64) {
    EXPECT_EQ(held<std::int64_t>(Value{42}), 42);
    EXPECT_EQ(held<std::int64_t>(Value{std::uint32_t{4'000'000'000}}), 4'000'000'000);
    EXPECT_EQ(held<std::int64_t>(Value{std::size_t{7}}), 7);
    EXPECT_EQ(held<std::int64_t>(Value{std::numeric_limits<std::int64_t>::min()}),
              std::numeric_limits<std::int64_t>::min());
    EXPECT_EQ(held<std::int64_t>(Value{true}), 1);
    EXPECT_EQ(held<std::int64_t>(Value{false}), 0);
}

TEST(Value, FloatingPointBecomesDouble) {
    EXPECT_DOUBLE_EQ(held<double>(Value{2.5}), 2.5);
    EXPECT_DOUBLE_EQ(held<double>(Value{0.5F}), 0.5);
}

TEST(Value, TextIsCopiedSoTheSourceMayChange) {
    std::string title = "original";
    const Value value{std::string_view{title}};
    title = "changed";
    EXPECT_EQ(held<std::string>(value), "original");
    EXPECT_EQ(held<std::string>(Value{"literal"}), "literal");
    EXPECT_EQ(held<std::string>(Value{VideoId{"dQw4w9WgXcQ"}}), "dQw4w9WgXcQ");
}

TEST(Value, BlobsFromVectorsAndSpans) {
    const Blob bytes{std::byte{1}, std::byte{2}};
    EXPECT_EQ(held<Blob>(Value{bytes}), bytes);
    EXPECT_EQ(held<Blob>(Value{std::span<const std::byte>{bytes}}), bytes);
    EXPECT_TRUE(held<Blob>(Value{Blob{}}).empty());
}

TEST(Value, NullComesFromNullptrDefaultAndEmptyOptionals) {
    EXPECT_TRUE(Value{}.is_null());
    EXPECT_TRUE(Value{nullptr}.is_null());
    EXPECT_TRUE(Value{std::optional<std::string>{}}.is_null());
    EXPECT_FALSE(Value{std::optional<std::string>{""}}.is_null());  // empty text is still text
    EXPECT_EQ(held<std::int64_t>(Value{std::optional<int>{5}}), 5);
}

TEST(ValueDeathTest, UnsignedValuesBeyondInt64Abort) {
    // Wrapping would silently store a different number; refusing is the only
    // safe answer. "threadsafe" re-runs the binary for the child, which works
    // under every sanitizer.
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH((void)Value{std::numeric_limits<std::uint64_t>::max()}, "does not fit");
}

}  // namespace
}  // namespace rr::db
