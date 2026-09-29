// rr/proc/process.hpp — running external programs safely.
//
// Why this exists
//   rReport drives many external tools: yt-dlp, ffmpeg and ffprobe on the
//   hub; llama-server, whisper-cli, rr-speech-worker and nvidia-smi on the pod
//   (ADR-0003: GPU engines are processes). This module is the only code that
//   starts processes, so the hard parts are solved once (ADR-0013):
//
//   * Process groups. Every child leads its own process group, and stopping
//     it signals the whole group. Stopping yt-dlp also stops the ffmpeg it
//     spawned. When a child exits, anything left in its group is killed:
//     no orphan keeps running, or keeps our output pipes open (which would
//     hang the reader).
//   * An escalating stop: SIGTERM to the group, a grace period, then SIGKILL.
//     Timeouts, cancellation and shutdown all use it.
//   * A clean start: stdin is /dev/null (ffmpeg would otherwise read
//     keystrokes from our terminal); every signal disposition is reset to
//     its default (a server ignores SIGPIPE, and its children must not
//     inherit that); no signal is blocked.
//   * Environment overrides replace inherited values rather than shadow them.
//   * Output is always drained, so a chatty child never blocks on a full
//     pipe. Captured output is capped.
//   * A non-zero exit status is data, not an error: tools use exit codes to
//     say things. Errors mean "could not run it at all", a timeout or a
//     cancellation.
//
// Thread-safety: run() and run_streaming() block the calling thread and may
//   be called from any number of threads. ChildProcess is thread-safe; its
//   callbacks run on the child's monitor thread. Callbacks must not throw.
//   rReport must never set SIGCHLD to SIG_IGN, which would make the kernel
//   reap children behind our back.
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/environment.hpp>
#include <rr/core/result.hpp>
#include <rr/proc/line_splitter.hpp>

namespace rr::proc {

struct Command {
    // argv[0] is an executable name looked up in PATH, or a path.
    std::vector<std::string> argv{};
    std::optional<std::filesystem::path> working_directory{};
    // Set in the child's environment, replacing inherited values of the same name.
    std::vector<EnvironmentVariable> environment{};
    // false: the child sees only `environment` (hermetic runs, tests).
    bool inherit_environment = true;
};

// How a process ended.
struct ExitStatus {
    int code = 0;                 // the exit code, or 128 + signal when killed (the shell convention)
    std::optional<int> signal{};  // the signal that ended it, if any

    [[nodiscard]] bool success() const noexcept { return !signal.has_value() && code == 0; }
    // "exit code 3", "killed by SIGKILL"
    [[nodiscard]] std::string describe() const;

    friend bool operator==(const ExitStatus&, const ExitStatus&) = default;
};

struct RunOptions {
    // Stop the child (SIGTERM, grace, SIGKILL) and fail with Errc::timeout.
    std::optional<std::chrono::milliseconds> timeout{};
    // How long SIGTERM has to work before SIGKILL follows.
    std::chrono::milliseconds kill_grace{3000};
    // Per stream. Output beyond the cap is read (the child must not block)
    // but dropped, and the result says so.
    std::size_t max_output_bytes = std::size_t{16} << 20U;
};

struct RunResult {
    ExitStatus status{};
    std::string stdout_text{};
    std::string stderr_text{};
    bool stdout_truncated = false;
    bool stderr_truncated = false;
    std::chrono::milliseconds duration{0};

    // The end of stderr (at most `max_bytes`, starting at a line boundary):
    // what to show a user when a tool fails.
    [[nodiscard]] std::string_view stderr_tail(std::size_t max_bytes = 2048) const noexcept;
};

// Runs `command` to completion and captures its output. Fails with
// Errc::not_found (no such executable), Errc::permission_denied,
// Errc::timeout or Errc::cancelled; the child is always stopped and reaped
// before this returns.
[[nodiscard]] Result<RunResult> run(const Command& command, const RunOptions& options = {},
                                    const std::stop_token& stop = {});

// Like run(), but delivers output line by line as it arrives (for progress),
// instead of capturing it. Either callback may be empty.
[[nodiscard]] Result<ExitStatus> run_streaming(const Command& command, const LineCallback& on_stdout,
                                               const LineCallback& on_stderr, const RunOptions& options = {},
                                               const std::stop_token& stop = {});

// The absolute path of an executable: `name` itself if it contains a '/',
// otherwise the first match in PATH. Errc::not_found when there is none.
[[nodiscard]] Result<std::filesystem::path> find_executable(std::string_view name);

// A long-running child, such as a llama-server engine: started, monitored
// from a background thread, and stopped on request or on destruction.
class ChildProcess {
public:
    struct Options {
        // Every line of stdout and stderr (e.g. into the log). Monitor thread.
        LineCallback on_output{};
        // Called once when the child has ended and been reaped. Monitor thread.
        std::function<void(const ExitStatus&)> on_exit{};
        // How long SIGTERM has to work before SIGKILL follows.
        std::chrono::milliseconds stop_grace{10'000};
    };

    // Errc::not_found / Errc::permission_denied when it cannot start at all.
    [[nodiscard]] static Result<ChildProcess> start(const Command& command, Options options);

    ChildProcess(ChildProcess&&) noexcept;
    ChildProcess& operator=(ChildProcess&&) noexcept;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    // Stops the child (and its group) if it is still running, and waits.
    ~ChildProcess();

    [[nodiscard]] int pid() const noexcept;
    [[nodiscard]] bool running() const;
    // Set once the child has ended.
    [[nodiscard]] std::optional<ExitStatus> exit_status() const;
    // Waits for the child to end on its own; Errc::timeout if it is still
    // running after `timeout`.
    [[nodiscard]] Result<ExitStatus> wait(std::chrono::milliseconds timeout) const;
    // SIGTERM to the group, SIGKILL after the grace period; waits for the end.
    ExitStatus stop();

private:
    struct State;
    explicit ChildProcess(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;
};

}  // namespace rr::proc
