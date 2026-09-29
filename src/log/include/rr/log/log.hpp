// rr/log/log.hpp — structured logging for the hub and the runtime.
//
// Why this exists
//   A pipeline run spans threads, stages, missions and two machines. A log
//   line is only useful if it says *which* job, stage and mission it belongs
//   to, and if that context is machine-readable. rr_log provides:
//     * levels, and two sinks: a human console format and rotating JSON Lines
//       files;
//     * structured context: key/value fields attached to every line a thread
//       logs while a ScopedField is alive, e.g.
//         log::ScopedField job{"job", job_id.value()};
//         log::info("stage {} started", name);
//       produces {"ts":…,"level":"info","msg":"stage atomize started","job":"01J…"};
//     * context propagation: capture the fields on one thread with
//       context_snapshot() and restore them on another with ScopedContext, so a
//       task on a thread pool logs with the context of whoever submitted it.
//
// Why not spdlog (as the plan first suggested)
//   Carrying structured, per-thread context into JSON output needs spdlog's
//   formatter internals, which differ between versions. The requirements here
//   are small and stable, so a focused implementation is simpler and removes
//   two dependencies (spdlog, fmt).
//
// Global state
//   The logger is the one process-wide singleton the architecture allows
//   (docs/architecture.md §4). All functions are thread-safe; the configured
//   sinks are invoked under a mutex, so their output lines never interleave.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/core/result.hpp>

namespace rr::log {

enum class Level : std::uint8_t {
    trace,
    debug,
    info,
    warn,
    error,
    off
};

[[nodiscard]] std::string_view to_string(Level level) noexcept;
[[nodiscard]] std::optional<Level> level_from_string(std::string_view name) noexcept;

struct Field {
    std::string key;
    std::string value;

    friend bool operator==(const Field&, const Field&) = default;
};

// One log event as seen by sinks.
struct Record {
    std::chrono::system_clock::time_point time;
    Level level = Level::info;
    std::string_view message;
    std::span<const Field> fields;  // context fields, outermost first
};

class Sink {
public:
    Sink() = default;
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    Sink(Sink&&) = delete;
    Sink& operator=(Sink&&) = delete;
    virtual ~Sink() = default;

    // Called with the logger's mutex held: implementations need no locking of their own.
    virtual void write(const Record& record) = 0;
    virtual void flush() = 0;
};

// Human-readable lines on stderr:
//   2026-09-29T18:04:05.123Z INFO  stage atomize started  job=01J… stage=atomize
[[nodiscard]] std::unique_ptr<Sink> make_console_sink(bool color);

// JSON Lines, one object per record, rotated when the file exceeds
// `max_bytes`: file → file.1 → … → file.<max_files> (the oldest is deleted).
[[nodiscard]] Result<std::unique_ptr<Sink>> make_jsonl_file_sink(const std::filesystem::path& path,
                                                                 std::uintmax_t max_bytes = 20U << 20U,
                                                                 unsigned max_files = 5);

// Keeps records in memory; for tests.
class MemorySink final : public Sink {
public:
    struct Entry {
        Level level;
        std::string message;
        std::vector<Field> fields;
    };

    void write(const Record& record) override;
    void flush() override {}

    // Safe to call only while no other thread is logging (tests call it after the fact).
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }

private:
    std::vector<Entry> entries_;
};

// --- Configuration ----------------------------------------------------------

struct Options {
    Level level = Level::info;
    bool console = true;
    bool color = true;                 // only honored when stderr is a terminal
    std::filesystem::path jsonl_file;  // empty: no file sink
};

// Replaces the logger's sinks and level according to `options`.
[[nodiscard]] Result<void> init(const Options& options);

// Replaces all sinks (used by init() and by tests).
void set_sinks(std::vector<std::unique_ptr<Sink>> sinks);
void set_level(Level level) noexcept;
[[nodiscard]] Level level() noexcept;
[[nodiscard]] bool enabled(Level level) noexcept;
void flush();

// --- Context -----------------------------------------------------------------

// Adds `key=value` to this thread's context for the lifetime of the object.
class ScopedField {
public:
    ScopedField(std::string key, std::string value);
    ScopedField(const ScopedField&) = delete;
    ScopedField& operator=(const ScopedField&) = delete;
    ScopedField(ScopedField&&) = delete;
    ScopedField& operator=(ScopedField&&) = delete;
    ~ScopedField();
};

// A copy of this thread's current context fields.
[[nodiscard]] std::vector<Field> context_snapshot();

// Installs `fields` as this thread's context for the object's lifetime and
// restores the previous context afterwards. Use it at the top of a task that
// runs on a pool thread, with a snapshot taken where the task was submitted.
class ScopedContext {
public:
    explicit ScopedContext(std::vector<Field> fields);
    ScopedContext(const ScopedContext&) = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;
    ScopedContext(ScopedContext&&) = delete;
    ScopedContext& operator=(ScopedContext&&) = delete;
    ~ScopedContext();

private:
    std::vector<Field> previous_;
};

// --- Emitting ------------------------------------------------------------------

void write(Level level, std::string_view message);

template <class... Args>
void trace(std::format_string<Args...> format, Args&&... args) {
    if (enabled(Level::trace)) {
        write(Level::trace, std::format(format, std::forward<Args>(args)...));
    }
}

template <class... Args>
void debug(std::format_string<Args...> format, Args&&... args) {
    if (enabled(Level::debug)) {
        write(Level::debug, std::format(format, std::forward<Args>(args)...));
    }
}

template <class... Args>
void info(std::format_string<Args...> format, Args&&... args) {
    if (enabled(Level::info)) {
        write(Level::info, std::format(format, std::forward<Args>(args)...));
    }
}

template <class... Args>
void warn(std::format_string<Args...> format, Args&&... args) {
    if (enabled(Level::warn)) {
        write(Level::warn, std::format(format, std::forward<Args>(args)...));
    }
}

template <class... Args>
void error(std::format_string<Args...> format, Args&&... args) {
    if (enabled(Level::error)) {
        write(Level::error, std::format(format, std::forward<Args>(args)...));
    }
}

}  // namespace rr::log
