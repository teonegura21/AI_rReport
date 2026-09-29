#include <rr/core/scope_exit.hpp>

#include <gtest/gtest.h>

namespace rr {
namespace {

TEST(ScopeExit, RunsActionWhenLeavingScope) {
    int calls = 0;
    {
        const ScopeExit guard{[&] { ++calls; }};
        EXPECT_EQ(calls, 0);
    }
    EXPECT_EQ(calls, 1);
}

TEST(ScopeExit, ReleasedGuardDoesNothing) {
    int calls = 0;
    {
        ScopeExit guard{[&] { ++calls; }};
        guard.release();
    }
    EXPECT_EQ(calls, 0);
}

}  // namespace
}  // namespace rr
