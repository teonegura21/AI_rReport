// rr/core/environment.hpp — a snapshot of the process environment.
//
// Why this exists
//   Two modules need the full environment rather than single getenv()
//   lookups: rr_config maps RREPORT_* variables onto configuration keys, and
//   rr_proc builds a child's environment with overrides applied. Reading
//   `environ` portably takes a platform switch (macOS hides it behind
//   _NSGetEnviron), which belongs in one place.
//
// Thread-safety: reading the environment races with setenv()/putenv() in
//   another thread. rReport never modifies its own environment after
//   startup, which makes snapshots safe.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rr {

using EnvironmentVariable = std::pair<std::string, std::string>;  // name, value

// Every NAME=VALUE entry of the current process environment, in order.
// Entries without '=' (which POSIX does not forbid) are skipped.
[[nodiscard]] std::vector<EnvironmentVariable> environment_snapshot();

// The value of one variable; std::nullopt when it is not set. A thread-safe
// stand-in for getenv() under the same rule: nobody modifies the environment.
[[nodiscard]] std::optional<std::string> environment_variable(std::string_view name);

}  // namespace rr
