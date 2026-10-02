// cli.hpp — what rreport's commands share: the exit codes, the global
// options, the hub they load, and how a job is taken through its stages and
// told. main.cpp holds the commands, and eval_commands.cpp the eval's.
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/config/config.hpp>
#include <rr/core/error.hpp>
#include <rr/core/result.hpp>
#include <rr/db/database.hpp>
#include <rr/hub/hub_config.hpp>
#include <rr/library/jobs.hpp>
#include <rr/net/http_client.hpp>
#include <rr/pipeline/job_run.hpp>
#include <rr/proc/termination_signals.hpp>

namespace rr::cli {

// Exit codes (main.cpp's header). Plain ints rather than an enum: the set is
// open, since 128 + N for signal N is computed.
inline constexpr int kOk = 0;
inline constexpr int kFailure = 1;
inline constexpr int kBadConfiguration = 2;

// Options shared by every command.
struct Global {
    std::optional<std::filesystem::path> config_file;
    std::optional<std::filesystem::path> secrets_file;
    std::vector<std::string> overrides;
};

// The hub's configuration, and the settings read from it.
struct Hub {
    config::Config config;
    hub::HubSettings settings;
};

// Prints an error for people and returns `code`.
int failed(int code, const Error& error);

// A command stopped by a signal exits 128 + its number, the shell convention.
int interrupted_or(int code, const Error& error, const proc::TerminationSignals& signals);

// The configuration: the defaults, the files, the environment, `--set`.
[[nodiscard]] Result<config::Config> load_config(const Global& global);

// The configuration and the settings read from it, the log started.
[[nodiscard]] Result<Hub> load(const Global& global);

[[nodiscard]] net::HttpClient make_http();

// A line of progress, for the user to read (stderr).
void say(std::string_view line);

// "2026-10-01 20:31 UTC"
[[nodiscard]] std::string when(std::chrono::system_clock::time_point at);

// "4.2 s", "3 min 12 s", "1 h 05 min"
[[nodiscard]] std::string took(std::chrono::milliseconds elapsed);

// How long a job's video lasts, asked by its army when it is raised: the
// job's ceilings are per hour of it (rr/hub/job_army.hpp).
[[nodiscard]] std::function<Result<std::chrono::milliseconds>()> video_length_of(db::Database& database,
                                                                                 std::string job_id);

// What a job's run did, and what to do next, for the user; the exit code
// that says it. `again` is the command that takes the job up again.
int report_run(const pipeline::JobRun& run, std::string_view again, const proc::TerminationSignals& signals);

// Takes `job`, an article job, through the stages, raising the army only if
// a stage asks for it; says what it did and what to do next, and returns
// the exit code that says it.
int run_job(const Hub& hub, db::Database& database, library::Job job,
            const proc::TerminationSignals& signals);

}  // namespace rr::cli
