#include <rr/proc/process.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <exception>
#include <format>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <rr/core/strings.hpp>
#include <rr/core/unique_fd.hpp>

namespace rr::proc {

namespace {

using Clock = std::chrono::steady_clock;

// How often a quiet child's pump wakes to check for stop requests and exit.
// Ten wake-ups a second cost nothing measurable, and a stop is noticed within
// 100 ms; an event-driven design (pidfd plus a wake-up pipe) would add moving
// parts for no benefit we need.
constexpr std::chrono::milliseconds kSlice{100};
// Once the leader has exited and its group was killed, the pipes reach EOF
// almost at once. This bounds the wait if a process outside the group (one
// that called setsid()) still holds them.
constexpr std::chrono::milliseconds kDrainLimit{1000};
constexpr std::size_t kReadChunk = 16384;

std::unexpected<Error> os_error(Errc code, std::string_view what, int error_number,
                                std::source_location where = std::source_location::current()) {
    return fail(code, std::format("{}: {}", what, std::system_category().message(error_number)), where);
}

Errc classify_spawn_error(int error_number) noexcept {
    switch (error_number) {
        case ENOENT:
        case ENOTDIR:
            return Errc::not_found;
        case EACCES:
        case EPERM:
            return Errc::permission_denied;
        default:
            return Errc::io;
    }
}

int milliseconds_until(Clock::time_point deadline) noexcept {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    return static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 0, kSlice.count()));
}

ExitStatus to_exit_status(int wait_status) noexcept {
    if (WIFEXITED(wait_status)) {
        return ExitStatus{.code = WEXITSTATUS(wait_status), .signal = std::nullopt};
    }
    if (WIFSIGNALED(wait_status)) {
        const int signal_number = WTERMSIG(wait_status);
        return ExitStatus{.code = 128 + signal_number, .signal = signal_number};
    }
    return ExitStatus{.code = -1, .signal = std::nullopt};  // stopped/continued: not requested, cannot occur
}

// ---------------------------------------------------------------------------
// Pipes and posix_spawn plumbing
// ---------------------------------------------------------------------------
struct Pipe {
    UniqueFd read;
    UniqueFd write;
};

// Both ends are close-on-exec, so a child spawned concurrently by another
// thread cannot inherit them (which would delay our EOF until it exits).
Result<Pipe> make_pipe() {
    std::array<int, 2> fds{-1, -1};
#if defined(__linux__)
    if (::pipe2(fds.data(), O_CLOEXEC) != 0) {
        return os_error(Errc::io, "cannot create a pipe", errno);
    }
#else
    if (::pipe(fds.data()) != 0) {
        return os_error(Errc::io, "cannot create a pipe", errno);
    }
    for (const int fd : fds) {
        (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
#endif
    return Pipe{.read = UniqueFd{fds[0]}, .write = UniqueFd{fds[1]}};
}

Result<void> check_spawn_setup(int result, std::string_view what) {
    if (result != 0) {
        return os_error(Errc::io, std::format("cannot prepare the child ({})", what), result);
    }
    return ok();
}

class SpawnFileActions {
public:
    SpawnFileActions() noexcept : initialized_(posix_spawn_file_actions_init(&actions_) == 0) {}
    SpawnFileActions(const SpawnFileActions&) = delete;
    SpawnFileActions& operator=(const SpawnFileActions&) = delete;
    SpawnFileActions(SpawnFileActions&&) = delete;
    SpawnFileActions& operator=(SpawnFileActions&&) = delete;
    ~SpawnFileActions() {
        if (initialized_) {
            posix_spawn_file_actions_destroy(&actions_);
        }
    }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] posix_spawn_file_actions_t* get() noexcept { return &actions_; }

private:
    posix_spawn_file_actions_t actions_{};
    bool initialized_ = false;
};

class SpawnAttributes {
public:
    SpawnAttributes() noexcept : initialized_(posix_spawnattr_init(&attributes_) == 0) {}
    SpawnAttributes(const SpawnAttributes&) = delete;
    SpawnAttributes& operator=(const SpawnAttributes&) = delete;
    SpawnAttributes(SpawnAttributes&&) = delete;
    SpawnAttributes& operator=(SpawnAttributes&&) = delete;
    ~SpawnAttributes() {
        if (initialized_) {
            posix_spawnattr_destroy(&attributes_);
        }
    }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] posix_spawnattr_t* get() noexcept { return &attributes_; }

private:
    posix_spawnattr_t attributes_{};
    bool initialized_ = false;
};

// NAME=VALUE entries: the parent's environment (when inherited) with the
// command's variables replacing same-named ones. Appending instead would
// leave the parent's value first, and getenv() returns the first match.
std::vector<std::string> build_environment(const Command& command) {
    std::vector<EnvironmentVariable> variables;
    if (command.inherit_environment) {
        variables = environment_snapshot();
    }
    for (const auto& [name, value] : command.environment) {
        if (const auto existing = std::ranges::find(variables, name, &EnvironmentVariable::first);
            existing != variables.end()) {
            existing->second = value;
        } else {
            variables.emplace_back(name, value);
        }
    }
    std::vector<std::string> entries;
    entries.reserve(variables.size());
    for (const auto& [name, value] : variables) {
        entries.push_back(std::format("{}={}", name, value));
    }
    return entries;
}

// posix_spawn() takes `char* const[]` (a historical signature; it never
// modifies the strings). Non-const std::string::data() avoids a const_cast.
std::vector<char*> null_terminated(std::vector<std::string>& strings) {
    std::vector<char*> pointers;
    pointers.reserve(strings.size() + 1);
    for (std::string& text : strings) {
        pointers.push_back(text.data());
    }
    pointers.push_back(nullptr);
    return pointers;
}

// ---------------------------------------------------------------------------
// Child: a spawned, not yet reaped process
// ---------------------------------------------------------------------------
// Owns the read ends of the child's stdout and stderr, and never outlives the
// process: the destructor kills the whole group and reaps it, whatever path
// (including an exception from a callback) led there.
//
// The child leads its own process group, whose ID equals its PID. The group
// may be signalled for as long as the leader is not reaped: until then even
// a dead (zombie) leader keeps the ID reserved, so a signal can never reach
// an unrelated process that reused it.
class Child {
public:
    Child(pid_t pid, UniqueFd out, UniqueFd err) noexcept
        : pid_(pid)
        , streams_{std::move(out), std::move(err)} {}
    Child(Child&& other) noexcept
        : pid_(std::exchange(other.pid_, -1))
        , streams_(std::move(other.streams_)) {}
    Child& operator=(Child&&) = delete;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child() {
        if (pid_ > 0) {
            (void)kill_and_reap();
        }
    }

    [[nodiscard]] pid_t pid() const noexcept { return pid_; }
    [[nodiscard]] int stream(std::size_t index) const noexcept { return streams_.at(index).get(); }
    void close_stream(std::size_t index) noexcept { streams_.at(index).reset(); }

    void signal_group(int signal_number) const noexcept {
        if (pid_ <= 0) {
            return;
        }
        if (::kill(-pid_, signal_number) != 0 && errno == ESRCH) {
            (void)::kill(pid_, signal_number);  // no such group: the leader left it; signal the leader alone
        }
    }

    // Whether the leader has exited, without reaping it (WNOWAIT).
    [[nodiscard]] bool has_exited() const noexcept {
        siginfo_t info{};
        if (::waitid(P_PID, static_cast<id_t>(pid_), &info, WEXITED | WNOHANG | WNOWAIT) != 0) {
            return errno != EINTR;  // ECHILD: already gone; report it as exited so reap() can conclude
        }
        return info.si_pid != 0;
    }

    ExitStatus reap() noexcept {
        int wait_status = 0;
        pid_t result = -1;
        do {
            result = ::waitpid(pid_, &wait_status, 0);
        } while (result < 0 && errno == EINTR);
        pid_ = -1;
        if (result < 0) {
            return ExitStatus{.code = -1, .signal = std::nullopt};  // reaped elsewhere (SIGCHLD ignored?)
        }
        return to_exit_status(wait_status);
    }

    ExitStatus kill_and_reap() noexcept {
        signal_group(SIGKILL);
        return reap();
    }

private:
    pid_t pid_;
    std::array<UniqueFd, 2> streams_;  // [0] stdout, [1] stderr
};

// The child's descriptors: stdin from /dev/null (a child must never read our
// terminal, or block on it), stdout and stderr into our pipes, and the
// working directory. dup2() clears close-on-exec on the new descriptors;
// the originals close at exec.
Result<void> prepare_file_actions(SpawnFileActions& actions, const Pipe& out, const Pipe& err,
                                  const std::string& working_directory) {
    RR_TRY(check_spawn_setup(actions.initialized() ? 0 : ENOMEM, "file actions"));
    RR_TRY(check_spawn_setup(
        posix_spawn_file_actions_addopen(actions.get(), STDIN_FILENO, "/dev/null", O_RDONLY, 0), "stdin"));
    RR_TRY(check_spawn_setup(posix_spawn_file_actions_adddup2(actions.get(), out.write.get(), STDOUT_FILENO),
                             "stdout"));
    RR_TRY(check_spawn_setup(posix_spawn_file_actions_adddup2(actions.get(), err.write.get(), STDERR_FILENO),
                             "stderr"));
    if (!working_directory.empty()) {
        RR_TRY(
            check_spawn_setup(posix_spawn_file_actions_addchdir_np(actions.get(), working_directory.c_str()),
                              "working directory"));
    }
    return ok();
}

// The child's process state: a process group of its own, every signal
// disposition at its default, and nothing blocked (ADR-0013).
Result<void> prepare_attributes(SpawnAttributes& attributes) {
    RR_TRY(check_spawn_setup(attributes.initialized() ? 0 : ENOMEM, "attributes"));
    sigset_t defaults{};
    sigfillset(&defaults);
    sigdelset(&defaults, SIGKILL);  // these two cannot be caught, so there is nothing to reset
    sigdelset(&defaults, SIGSTOP);
    sigset_t unblocked{};
    sigemptyset(&unblocked);
    RR_TRY(check_spawn_setup(posix_spawnattr_setsigdefault(attributes.get(), &defaults), "signal defaults"));
    RR_TRY(check_spawn_setup(posix_spawnattr_setsigmask(attributes.get(), &unblocked), "signal mask"));
    RR_TRY(
        check_spawn_setup(posix_spawnattr_setpgroup(attributes.get(), 0), "process group"));  // 0: own group
    constexpr int kFlags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
    RR_TRY(
        check_spawn_setup(posix_spawnattr_setflags(attributes.get(), static_cast<short>(kFlags)), "flags"));
    return ok();
}

Result<void> check_working_directory(const Command& command) {
    if (!command.working_directory.has_value()) {
        return ok();
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(*command.working_directory, ec)) {
        return fail(Errc::not_found, std::format("working directory '{}' does not exist",
                                                 command.working_directory->string()));
    }
    return ok();
}

Result<Child> spawn(const Command& command) {
    if (command.argv.empty() || command.argv.front().empty()) {
        return fail(Errc::invalid_argument, "cannot run an empty command");
    }
    // Resolved up front: a clear "not found" beats a failed exec.
    RR_TRY_ASSIGN(const std::filesystem::path executable, find_executable(command.argv.front()));
    RR_TRY(check_working_directory(command));
    RR_TRY_ASSIGN(Pipe out, make_pipe());
    RR_TRY_ASSIGN(Pipe err, make_pipe());

    const std::string working_directory =
        command.working_directory.has_value() ? command.working_directory->string() : std::string{};
    SpawnFileActions actions;
    RR_TRY(prepare_file_actions(actions, out, err, working_directory));
    SpawnAttributes attributes;
    RR_TRY(prepare_attributes(attributes));

    std::vector<std::string> arguments = command.argv;
    std::vector<std::string> environment = build_environment(command);
    const std::vector<char*> argv = null_terminated(arguments);
    const std::vector<char*> envp = null_terminated(environment);

    pid_t pid = -1;
    const int result =
        posix_spawn(&pid, executable.c_str(), actions.get(), attributes.get(), argv.data(), envp.data());
    if (result != 0) {
        return os_error(classify_spawn_error(result), std::format("cannot start '{}'", executable.string()),
                        result);
    }
    // The write ends close here, in the parent: the child holds its own copies,
    // so EOF arrives exactly when the child side is done.
    return Child{pid, std::move(out.read), std::move(err.read)};
}

// ---------------------------------------------------------------------------
// The pump
// ---------------------------------------------------------------------------
struct PumpHooks {
    std::function<void(bool is_stderr, std::string_view bytes)> on_output;
    std::function<bool()> should_stop;  // polled; true starts the SIGTERM → SIGKILL escalation
    std::chrono::milliseconds grace{0};
};

// Drains the child's output until it ends, applies stop requests, kills
// whatever is left in its process group, and reaps it. It runs on one thread
// and is the only code touching the child after spawn(), so signalling and
// reaping can never race.
ExitStatus pump(Child& child, const PumpHooks& hooks) {
    std::array<pollfd, 2> fds{pollfd{.fd = child.stream(0), .events = POLLIN, .revents = 0},
                              pollfd{.fd = child.stream(1), .events = POLLIN, .revents = 0}};
    std::array<char, kReadChunk> buffer{};

    // Waits up to `timeout_ms` for output, and delivers whatever is ready.
    // A stream is closed at EOF, or on a read error (the child then gets
    // EPIPE instead of blocking on a pipe nobody reads).
    const auto service = [&](int timeout_ms) {
        if (::poll(fds.data(), fds.size(), timeout_ms) <= 0) {
            return;  // timeout, EINTR, or a transient failure: the caller loops
        }
        for (std::size_t index = 0; index < fds.size(); ++index) {
            pollfd& entry = fds.at(index);
            if (entry.fd < 0 || (entry.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                continue;
            }
            const ssize_t count = ::read(entry.fd, buffer.data(), buffer.size());
            if (count > 0) {
                hooks.on_output(index == 1, std::string_view{buffer.data(), static_cast<std::size_t>(count)});
            } else if (count == 0 || errno != EINTR) {
                child.close_stream(index);
                entry.fd = -1;  // poll() ignores negative descriptors
            }
        }
    };

    std::optional<Clock::time_point> terminate_sent;
    bool kill_sent = false;
    while (!child.has_exited()) {
        if (!terminate_sent.has_value()) {
            if (hooks.should_stop()) {
                child.signal_group(SIGTERM);
                terminate_sent = Clock::now();
            }
        } else if (!kill_sent && Clock::now() - *terminate_sent >= hooks.grace) {
            child.signal_group(SIGKILL);
            kill_sent = true;
        }
        service(static_cast<int>(kSlice.count()));
    }

    // The leader has exited; nothing left in its group may outlive it.
    child.signal_group(SIGKILL);
    const auto drain_until = Clock::now() + kDrainLimit;
    while ((fds[0].fd >= 0 || fds[1].fd >= 0) && Clock::now() < drain_until) {
        service(milliseconds_until(drain_until));
    }
    return child.reap();
}

void append_capped(std::string& text, bool& truncated, std::string_view bytes, std::size_t cap) {
    const std::size_t room = cap - std::min(cap, text.size());
    if (bytes.size() > room) {
        truncated = true;
        bytes = bytes.substr(0, room);
    }
    text.append(bytes);
}

enum class StopReason : std::uint8_t {
    none,
    timeout,
    cancelled
};

// The should_stop hook shared by run() and run_streaming().
class StopCondition {
public:
    StopCondition(std::stop_token stop, const RunOptions& options) noexcept
        : stop_(std::move(stop))
        , deadline_(options.timeout.has_value() ? Clock::now() + *options.timeout
                                                : Clock::time_point::max()) {}

    bool operator()() {
        if (stop_.stop_requested()) {
            reason_ = StopReason::cancelled;
        } else if (Clock::now() >= deadline_) {
            reason_ = StopReason::timeout;
        }
        return reason_ != StopReason::none;
    }

    [[nodiscard]] Result<void> as_result(const Command& command, const RunOptions& options) const {
        switch (reason_) {
            case StopReason::cancelled:
                return fail(Errc::cancelled, std::format("'{}' was cancelled", command.argv.front()));
            case StopReason::timeout:
                return fail(
                    Errc::timeout,
                    std::format("'{}' did not finish within {} ms and was stopped", command.argv.front(),
                                options.timeout.value_or(std::chrono::milliseconds{0}).count()));
            case StopReason::none:
                break;
        }
        return ok();
    }

private:
    std::stop_token stop_;
    Clock::time_point deadline_;
    StopReason reason_ = StopReason::none;
};

}  // namespace

std::string_view signal_name(int signal_number) noexcept {
    switch (signal_number) {
        case SIGHUP:
            return "SIGHUP";
        case SIGINT:
            return "SIGINT";
        case SIGQUIT:
            return "SIGQUIT";
        case SIGILL:
            return "SIGILL";
        case SIGABRT:
            return "SIGABRT";
        case SIGBUS:
            return "SIGBUS";
        case SIGFPE:
            return "SIGFPE";
        case SIGKILL:
            return "SIGKILL";
        case SIGSEGV:
            return "SIGSEGV";
        case SIGPIPE:
            return "SIGPIPE";
        case SIGALRM:
            return "SIGALRM";
        case SIGTERM:
            return "SIGTERM";
        default:
            return {};
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
std::string ExitStatus::describe() const {
    if (signal.has_value()) {
        const std::string_view name = signal_name(*signal);
        return name.empty() ? std::format("killed by signal {}", *signal) : std::format("killed by {}", name);
    }
    return std::format("exit code {}", code);
}

std::string_view RunResult::stderr_tail(std::size_t max_bytes) const noexcept {
    std::string_view text = stderr_text;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    if (text.size() <= max_bytes) {
        return text;
    }
    text = text.substr(text.size() - max_bytes);
    // Start at a line boundary when there is one, so the first line is whole.
    if (const auto newline = text.find('\n');
        newline != std::string_view::npos && newline + 1 < text.size()) {
        text.remove_prefix(newline + 1);
    }
    return text;
}

Result<std::filesystem::path> find_executable(std::string_view name) {
    if (name.empty()) {
        return fail(Errc::invalid_argument, "empty executable name");
    }
    const auto usable = [](const std::filesystem::path& candidate) {
        std::error_code ec;
        return std::filesystem::is_regular_file(candidate, ec) && ::access(candidate.c_str(), X_OK) == 0;
    };
    if (name.find('/') != std::string_view::npos) {
        const std::filesystem::path path{name};
        if (usable(path)) {
            return std::filesystem::absolute(path);
        }
        return fail(Errc::not_found, std::format("'{}' is not an executable file", name));
    }
    // Searched in *our* PATH, like posix_spawnp(), even when the child gets
    // a different environment.
    const std::string directories = environment_variable("PATH").value_or("/usr/local/bin:/usr/bin:/bin");
    for (const std::string_view directory : split(directories, ':')) {
        // POSIX: an empty entry means the current directory.
        const std::filesystem::path candidate =
            std::filesystem::path{directory.empty() ? std::string_view{"."} : directory} / name;
        if (usable(candidate)) {
            return std::filesystem::absolute(candidate);
        }
    }
    return fail(Errc::not_found, std::format("'{}' was not found in PATH", name));
}

Result<RunResult> run(const Command& command, const RunOptions& options, const std::stop_token& stop) {
    RR_TRY_ASSIGN(Child child, spawn(command));
    const auto started = Clock::now();
    RunResult result;
    StopCondition stop_condition{stop, options};
    const PumpHooks hooks{
        .on_output =
            [&](bool is_stderr, std::string_view bytes) {
                if (is_stderr) {
                    append_capped(result.stderr_text, result.stderr_truncated, bytes,
                                  options.max_output_bytes);
                } else {
                    append_capped(result.stdout_text, result.stdout_truncated, bytes,
                                  options.max_output_bytes);
                }
            },
        .should_stop = [&] { return stop_condition(); },
        .grace = options.kill_grace,
    };
    result.status = pump(child, hooks);
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    RR_TRY(stop_condition.as_result(command, options));
    return result;
}

Result<ExitStatus> run_streaming(const Command& command, const LineCallback& on_stdout,
                                 const LineCallback& on_stderr, const RunOptions& options,
                                 const std::stop_token& stop) {
    RR_TRY_ASSIGN(Child child, spawn(command));
    LineSplitter out{on_stdout};
    LineSplitter err{on_stderr};
    StopCondition stop_condition{stop, options};
    const PumpHooks hooks{
        .on_output = [&](bool is_stderr, std::string_view bytes) { (is_stderr ? err : out).feed(bytes); },
        .should_stop = [&] { return stop_condition(); },
        .grace = options.kill_grace,
    };
    const ExitStatus status = pump(child, hooks);
    out.finish();
    err.finish();
    RR_TRY(stop_condition.as_result(command, options));
    return status;
}

// ---------------------------------------------------------------------------
// ChildProcess
// ---------------------------------------------------------------------------
struct ChildProcess::State {
    int pid = -1;
    mutable std::mutex mutex;
    mutable std::condition_variable ended_cv;
    bool ended = false;  // set once, when the child has been reaped
    ExitStatus status;   // valid once `ended`
    // Declared last, so it is destroyed first: its destructor requests a stop
    // and joins the monitor, which still uses the members above.
    std::jthread monitor;
};

ChildProcess::ChildProcess(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
ChildProcess::ChildProcess(ChildProcess&&) noexcept = default;
ChildProcess& ChildProcess::operator=(ChildProcess&&) noexcept = default;
ChildProcess::~ChildProcess() = default;

Result<ChildProcess> ChildProcess::start(const Command& command, Options options) {
    RR_TRY_ASSIGN(Child child, spawn(command));
    auto state = std::make_unique<State>();
    state->pid = child.pid();
    State* shared = state.get();  // outlives the thread: the thread is a member of *shared, joined first

    state->monitor = std::jthread{[shared, child = std::move(child),
                                   options = std::move(options)](const std::stop_token& stop) mutable {
        ExitStatus status;
        try {
            LineSplitter out{options.on_output};
            LineSplitter err{options.on_output};
            const PumpHooks hooks{
                .on_output = [&](bool is_stderr,
                                 std::string_view bytes) { (is_stderr ? err : out).feed(bytes); },
                .should_stop = [&stop] { return stop.stop_requested(); },
                .grace = options.stop_grace,
            };
            status = pump(child, hooks);
            out.finish();
            err.finish();
        } catch (const std::exception& error) {
            // A callback threw: a bug, but the child must still not outlive us.
            (void)std::fprintf(stderr, "rr::proc: output callback of process %d threw: %s\n", shared->pid,
                               error.what());
            status = child.kill_and_reap();
        } catch (...) {
            (void)std::fprintf(stderr, "rr::proc: output callback of process %d threw\n", shared->pid);
            status = child.kill_and_reap();
        }
        {
            const std::lock_guard lock(shared->mutex);
            shared->status = status;
            shared->ended = true;
        }
        shared->ended_cv.notify_all();
        if (options.on_exit) {
            options.on_exit(status);
        }
    }};
    return ChildProcess{std::move(state)};
}

int ChildProcess::pid() const noexcept {
    return state_->pid;
}

bool ChildProcess::running() const {
    const std::lock_guard lock(state_->mutex);
    return !state_->ended;
}

std::optional<ExitStatus> ChildProcess::exit_status() const {
    const std::lock_guard lock(state_->mutex);
    if (!state_->ended) {
        return std::nullopt;
    }
    return state_->status;
}

Result<ExitStatus> ChildProcess::wait(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(state_->mutex);
    if (!state_->ended_cv.wait_for(lock, timeout, [this] { return state_->ended; })) {
        return fail(Errc::timeout,
                    std::format("process {} is still running after {} ms", state_->pid, timeout.count()));
    }
    return state_->status;
}

ExitStatus ChildProcess::stop() {
    state_->monitor.request_stop();
    std::unique_lock lock(state_->mutex);
    state_->ended_cv.wait(lock, [this] { return state_->ended; });
    return state_->status;
}

}  // namespace rr::proc
