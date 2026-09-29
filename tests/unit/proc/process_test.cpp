#include <rr/proc/process.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <rr/core/strings.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::proc {
namespace {

using namespace std::chrono_literals;
using testkit::unwrap;
using Clock = std::chrono::steady_clock;

Command shell(std::string script) {
    return Command{.argv = {"sh", "-c", std::move(script)}};
}

// Whether `pid` is a live process (not gone, not a zombie awaiting reaping).
bool alive(int pid) {
    if (::kill(pid, 0) != 0) {
        return false;
    }
    std::ifstream stat{"/proc/" + std::to_string(pid) + "/stat"};
    std::string content;
    std::getline(stat, content);
    const auto close_paren = content.rfind(')');
    if (close_paren == std::string::npos || close_paren + 2 >= content.size()) {
        return true;  // no /proc (not Linux): kill() succeeding is all we know
    }
    const char state = content[close_paren + 2];
    return state != 'Z' && state != 'X';
}

bool eventually_dead(int pid, std::chrono::milliseconds within = 3s) {
    const auto until = Clock::now() + within;
    while (alive(pid)) {
        if (Clock::now() > until) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

// Collects lines from a monitor thread.
class LineLog {
public:
    LineCallback callback() {
        return [this](std::string_view line) {
            const std::lock_guard lock(mutex_);
            lines_.emplace_back(line);
        };
    }
    [[nodiscard]] bool contains(std::string_view line) const {
        const std::lock_guard lock(mutex_);
        return std::ranges::find(lines_, line) != lines_.end();
    }
    [[nodiscard]] bool wait_for(std::string_view line, std::chrono::milliseconds within = 5s) const {
        const auto until = Clock::now() + within;
        while (!contains(line)) {
            if (Clock::now() > until) {
                return false;
            }
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

// ---------------------------------------------------------------------------
// run()
// ---------------------------------------------------------------------------
TEST(Run, CapturesOutputAndTreatsExitCodesAsData) {
    const RunResult result = unwrap(run(shell("echo out; echo err >&2; exit 3")));
    EXPECT_EQ(result.stdout_text, "out\n");
    EXPECT_EQ(result.stderr_text, "err\n");
    EXPECT_EQ(result.status, (ExitStatus{.code = 3, .signal = std::nullopt}));
    EXPECT_FALSE(result.status.success());
    EXPECT_EQ(result.status.describe(), "exit code 3");
    EXPECT_TRUE(unwrap(run(shell("true"))).status.success());
}

TEST(Run, ReportsDeathBySignal) {
    const RunResult result = unwrap(run(shell("kill -9 $$")));
    EXPECT_EQ(result.status.signal, SIGKILL);
    EXPECT_EQ(result.status.code, 128 + SIGKILL);
    EXPECT_EQ(result.status.describe(), "killed by SIGKILL");
}

TEST(Run, CannotRunWhatDoesNotExist) {
    EXPECT_ERRC(run(Command{.argv = {"rr-no-such-tool-4f2a"}}), Errc::not_found);
    EXPECT_ERRC(run(Command{.argv = {"/nonexistent/tool"}}), Errc::not_found);
    EXPECT_ERRC(run(Command{}), Errc::invalid_argument);
    Command in_missing_directory = shell("true");
    in_missing_directory.working_directory = "/nonexistent/directory";
    EXPECT_ERRC(run(in_missing_directory), Errc::not_found);
}

TEST(Run, UsesTheWorkingDirectory) {
    const testkit::TempDir dir{"rr_proc_test"};
    Command command{.argv = {"pwd"}};
    command.working_directory = dir.path();
    EXPECT_EQ(unwrap(run(command)).stdout_text, std::filesystem::canonical(dir.path()).string() + "\n");
}

TEST(Run, EnvironmentOverridesReplaceInheritedValues) {
    // HOME is inherited. Appending "HOME=/overridden" after it would leave the
    // old value first, and getenv() returns the first match. `env` (not a
    // shell, which keeps the *last* duplicate) shows the raw environment.
    Command command{.argv = {"env"}};
    command.environment = {{"HOME", "/overridden"}, {"RR_TEST_VALUE", "hello"}};
    const std::string listing = unwrap(run(command)).stdout_text;
    std::vector<std::string> homes;
    for (const auto line : split(listing, '\n')) {
        if (line.starts_with("HOME=")) {
            homes.emplace_back(line);
        }
    }
    EXPECT_EQ(homes, (std::vector<std::string>{"HOME=/overridden"}));
    EXPECT_NE(listing.find("\nRR_TEST_VALUE=hello\n"), std::string::npos);
}

TEST(Run, AHermeticEnvironmentContainsOnlyWhatWasGiven) {
    Command command = shell(R"(echo "${HOME-unset} $ONLY")");
    command.inherit_environment = false;
    command.environment = {{"ONLY", "this"}};
    EXPECT_EQ(unwrap(run(command)).stdout_text, "unset this\n");
}

TEST(Run, OutputBeyondTheCapIsDrainedAndDropped) {
    RunOptions options;
    options.max_output_bytes = 1000;
    // 3 MB: far more than a pipe buffer, so the child would block if we stopped reading.
    const RunResult result = unwrap(run(shell("head -c 3000000 /dev/zero"), options));
    EXPECT_TRUE(result.status.success());
    EXPECT_EQ(result.stdout_text.size(), 1000U);
    EXPECT_TRUE(result.stdout_truncated);
    EXPECT_FALSE(result.stderr_truncated);
}

TEST(Run, ATimeoutStopsTheChild) {
    RunOptions options;
    options.timeout = 200ms;
    const auto started = Clock::now();
    EXPECT_ERRC(run(shell("sleep 30"), options), Errc::timeout);
    EXPECT_LT(Clock::now() - started, 5s);
}

TEST(Run, TheStopEscalatesToSigkillWhenSigtermIsIgnored) {
    RunOptions options;
    options.timeout = 100ms;
    options.kill_grace = 200ms;
    const auto started = Clock::now();
    // Both the shell and its sleep ignore SIGTERM; only SIGKILL to the group ends them.
    EXPECT_ERRC(run(shell(R"(trap "" TERM; sleep 30; echo unreachable)"), options), Errc::timeout);
    EXPECT_LT(Clock::now() - started, 5s);
}

TEST(Run, NothingInTheProcessGroupOutlivesTheLeader) {
    // The shell starts a background grandchild that inherits our output pipes,
    // prints its PID and exits at once. Without group handling, the reader
    // would wait 30 s for the pipes to close, and the orphan would live on.
    const auto started = Clock::now();
    const RunResult result = unwrap(run(shell("sleep 30 & echo $!")));
    EXPECT_LT(Clock::now() - started, 5s);
    const int grandchild = std::stoi(result.stdout_text);
    EXPECT_TRUE(eventually_dead(grandchild))
        << "the orphaned grandchild " << grandchild << " is still running";
}

TEST(Run, CancellationStopsTheChild) {
    std::stop_source cancel;
    const std::jthread canceller{[&cancel] {
        std::this_thread::sleep_for(100ms);
        cancel.request_stop();
    }};
    const auto started = Clock::now();
    EXPECT_ERRC(run(shell("sleep 30"), {}, cancel.get_token()), Errc::cancelled);
    EXPECT_LT(Clock::now() - started, 5s);
}

TEST(Run, StdinIsDevNullWhateverTheParentHas) {
    // Give this test process a stdin that never reaches EOF: a pipe whose
    // write end we keep open. A child that inherited it would block in `cat`.
    std::array<int, 2> never_ends{-1, -1};
    ASSERT_EQ(::pipe(never_ends.data()), 0);
    const int saved_stdin = ::dup(STDIN_FILENO);
    ASSERT_GE(saved_stdin, 0);
    ASSERT_GE(::dup2(never_ends[0], STDIN_FILENO), 0);

    RunOptions options;
    options.timeout = 5s;
    const auto cat = run(shell("cat"), options);
    const auto link = run(shell("readlink /proc/self/fd/0"), options);

    ::dup2(saved_stdin, STDIN_FILENO);
    ::close(saved_stdin);
    ::close(never_ends[0]);
    ::close(never_ends[1]);
    ASSERT_OK(cat) << "an inherited stdin would have blocked `cat` until the timeout";
    EXPECT_EQ(cat->stdout_text, "");
    if (link.has_value() && link->status.success()) {  // Linux: see where fd 0 points
        EXPECT_EQ(link->stdout_text, "/dev/null\n");
    }
}

TEST(Run, CapturesEverythingWrittenBeforeTheChildExits) {
    // The child may exit before a single byte is read; what it wrote is still
    // in the pipe and must be drained. Repeated, because it is a race.
    // dd writes all 60000 bytes (less than a pipe holds) in one write() and
    // exits at once, so most of it is still unread when the exit is noticed.
    for (int round = 0; round < 20; ++round) {
        const RunResult result = unwrap(run(Command{.argv = {"dd", "if=/dev/zero", "bs=60000", "count=1"}}));
        ASSERT_EQ(result.stdout_text.size(), 60000U) << "round " << round;
    }
}

#if defined(__linux__)
// A signal set from the child's /proc/self/status, e.g. field "SigIgn".
// grep runs as the direct child: a shell in between would clear its own mask
// at startup and hide what the child really inherited.
std::uint64_t child_signal_set(std::string_view field) {
    const RunResult result =
        unwrap(run(Command{.argv = {"grep", std::format("^{}:", field), "/proc/self/status"}}));
    const auto separator = result.stdout_text.find_first_of("\t ");
    return std::stoull(result.stdout_text.substr(separator + 1), nullptr, 16);
}

std::uint64_t bit(int signal_number) {
    return std::uint64_t{1} << static_cast<unsigned>(signal_number - 1);
}

TEST(Run, ChildrenStartWithDefaultSignalHandling) {
    // Servers ignore SIGPIPE and threads may block signals; children must
    // inherit neither.
    const auto previous_pipe = std::signal(SIGPIPE, SIG_IGN);
    sigset_t block{};
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigset_t previous_mask{};
    ASSERT_EQ(pthread_sigmask(SIG_BLOCK, &block, &previous_mask), 0);

    const std::uint64_t ignored = child_signal_set("SigIgn");
    const std::uint64_t blocked = child_signal_set("SigBlk");

    EXPECT_EQ(pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr), 0);
    EXPECT_NE(std::signal(SIGPIPE, previous_pipe), SIG_ERR);
    EXPECT_EQ(ignored & bit(SIGPIPE), 0U) << "SIGPIPE must not stay ignored in children";
    EXPECT_EQ(blocked & bit(SIGUSR1), 0U) << "blocked signals must not be inherited";
}
#endif

// ---------------------------------------------------------------------------
// run_streaming() and helpers
// ---------------------------------------------------------------------------
TEST(RunStreaming, DeliversLinesAsTheyArrive) {
    std::vector<std::string> out;
    std::vector<std::string> err;
    const ExitStatus status = unwrap(run_streaming(
        shell(R"(printf 'progress 10%%\rprogress 90%%\rdone\n'; echo warning >&2; exit 4)"),
        [&](std::string_view line) { out.emplace_back(line); },
        [&](std::string_view line) { err.emplace_back(line); }));
    EXPECT_EQ(status.code, 4);
    EXPECT_EQ(out, (std::vector<std::string>{"progress 10%", "progress 90%", "done"}));
    EXPECT_EQ(err, (std::vector<std::string>{"warning"}));
}

TEST(RunStreaming, HonoursTimeouts) {
    RunOptions options;
    options.timeout = 100ms;
    EXPECT_ERRC(run_streaming(shell("sleep 30"), nullptr, nullptr, options), Errc::timeout);
}

TEST(RunResult, StderrTailStartsAtALineBoundary) {
    RunResult result;
    result.stderr_text = "first line\nsecond line\nthird line\n";
    EXPECT_EQ(result.stderr_tail(), "first line\nsecond line\nthird line");
    EXPECT_EQ(result.stderr_tail(15), "third line");
    EXPECT_EQ(RunResult{}.stderr_tail(), "");
}

TEST(FindExecutable, SearchesPathAndChecksExplicitPaths) {
    const std::filesystem::path sh = unwrap(find_executable("sh"));
    EXPECT_TRUE(sh.is_absolute());
    EXPECT_EQ(unwrap(find_executable(sh.string())), sh);
    EXPECT_ERRC(find_executable("rr-no-such-tool-4f2a"), Errc::not_found);
    EXPECT_ERRC(find_executable("/etc/hostname"), Errc::not_found) << "a file that is not executable";
    EXPECT_ERRC(find_executable(""), Errc::invalid_argument);
}

// ---------------------------------------------------------------------------
// ChildProcess
// ---------------------------------------------------------------------------
TEST(ChildProcess, RunsUntilStoppedAndReportsHowItEnded) {
    LineLog log;
    std::atomic<int> exits{0};
    ChildProcess child = unwrap(ChildProcess::start(
        shell("echo ready; exec sleep 30"),
        {.on_output = log.callback(), .on_exit = [&](const ExitStatus&) { ++exits; }, .stop_grace = 5s}));
    ASSERT_TRUE(log.wait_for("ready"));
    EXPECT_TRUE(child.running());
    EXPECT_EQ(child.exit_status(), std::nullopt);

    const ExitStatus status = child.stop();
    EXPECT_EQ(status.signal, SIGTERM);
    EXPECT_FALSE(child.running());
    EXPECT_EQ(child.exit_status(), status);
    EXPECT_EQ(child.stop(), status) << "stopping twice is harmless";
    EXPECT_EQ(exits.load(), 1);
}

TEST(ChildProcess, StopEscalatesToSigkill) {
    LineLog log;
    ChildProcess child = unwrap(ChildProcess::start(shell(R"(trap "" TERM; echo ready; sleep 30)"),
                                                    {.on_output = log.callback(), .stop_grace = 200ms}));
    ASSERT_TRUE(log.wait_for("ready"));
    const auto started = Clock::now();
    EXPECT_EQ(child.stop().signal, SIGKILL);
    EXPECT_LT(Clock::now() - started, 5s);
}

TEST(ChildProcess, WaitSeesANaturalExitOrTimesOut) {
    std::atomic<int> reported{-1};
    const ChildProcess child = unwrap(ChildProcess::start(
        shell("sleep 0.3; exit 7"), {.on_exit = [&](const ExitStatus& status) { reported = status.code; }}));
    EXPECT_ERRC(child.wait(20ms), Errc::timeout);
    EXPECT_EQ(unwrap(child.wait(10s)).code, 7);
    EXPECT_EQ(reported.load(), 7);
}

TEST(ChildProcess, DestroyingItStopsTheChild) {
    int pid = -1;
    {
        const ChildProcess child =
            unwrap(ChildProcess::start(Command{.argv = {"sleep", "30"}}, {.stop_grace = 2s}));
        pid = child.pid();
        EXPECT_TRUE(alive(pid));
    }
    EXPECT_FALSE(alive(pid)) << "the destructor stops and reaps the child";
}

TEST(ChildProcess, CannotStartWhatDoesNotExist) {
    EXPECT_ERRC(ChildProcess::start(Command{.argv = {"rr-no-such-tool-4f2a"}}, {}), Errc::not_found);
}

TEST(ChildProcess, AThrowingCallbackStopsTheChildInsteadOfTheProgram) {
    const ChildProcess child = unwrap(ChildProcess::start(
        shell("echo boom; exec sleep 30"),
        {.on_output = [](std::string_view) { throw std::runtime_error("callback bug"); }, .stop_grace = 2s}));
    const ExitStatus status = unwrap(child.wait(10s));
    EXPECT_EQ(status.signal, SIGKILL);
}

}  // namespace
}  // namespace rr::proc
