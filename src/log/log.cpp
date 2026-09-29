#include <rr/log/log.hpp>

#include <array>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>

#include <unistd.h>

#include <nlohmann/json.hpp>

namespace rr::log {
namespace {

constexpr std::array<std::string_view, 6> kLevelNames{"trace", "debug", "info", "warn", "error", "off"};

// --- Global logger state --------------------------------------------------------

struct Logger {
    std::mutex mutex;  // guards sinks and serializes sink calls
    std::vector<std::unique_ptr<Sink>> sinks;
    std::atomic<Level> level{Level::info};
};

Logger& logger() {
    // Leaked on purpose: logging must keep working during static destruction,
    // e.g. from destructors of other globals. The OS reclaims it at exit.
    static Logger* instance = [] {
        auto* created = new Logger{};  // NOLINT(cppcoreguidelines-owning-memory)
        created->sinks.push_back(make_console_sink(true));
        return created;
    }();
    return *instance;
}

// Context fields of the current thread, outermost first.
thread_local std::vector<Field> t_context;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// "2026-09-29T18:04:05.123Z" — UTC, millisecond precision, sortable.
std::string format_time(std::chrono::system_clock::time_point time) {
    const auto millis = std::chrono::floor<std::chrono::milliseconds>(time);
    return std::format("{:%FT%T}Z", millis);
}

// --- Console sink --------------------------------------------------------------------

class ConsoleSink final : public Sink {
public:
    explicit ConsoleSink(bool color) : color_(color && ::isatty(STDERR_FILENO) == 1) {}

    void write(const Record& record) override {
        std::string line = format_time(record.time);
        line += ' ';
        if (color_) {
            line += color_code(record.level);
        }
        line += std::format("{:<5}", to_upper(record.level));
        if (color_) {
            line += "\033[0m";
        }
        line += ' ';
        line += record.message;
        if (!record.fields.empty()) {
            line += " ";
            for (const Field& field : record.fields) {
                line += std::format(" {}={}", field.key, field.value);
            }
        }
        line += '\n';
        (void)std::fwrite(line.data(), 1, line.size(), stderr);
    }

    void flush() override { (void)std::fflush(stderr); }

private:
    static std::string_view color_code(Level level) {
        switch (level) {
            case Level::trace:
            case Level::debug:
                return "\033[2m";  // dim
            case Level::info:
                return "\033[32m";  // green
            case Level::warn:
                return "\033[33m";  // yellow
            case Level::error:
                return "\033[1;31m";  // bold red
            case Level::off:
                break;
        }
        return "";
    }

    static std::string to_upper(Level level) {
        std::string name{to_string(level)};
        for (char& c : name) {
            c = static_cast<char>(c - 'a' + 'A');
        }
        return name;
    }

    bool color_;
};

// --- JSON Lines file sink -------------------------------------------------------------

class JsonlFileSink final : public Sink {
public:
    JsonlFileSink(std::filesystem::path path, std::uintmax_t max_bytes, unsigned max_files)
        : path_(std::move(path))
        , max_bytes_(max_bytes)
        , max_files_(max_files) {}

    [[nodiscard]] Result<void> open() {
        std::error_code error;
        if (path_.has_parent_path()) {
            std::filesystem::create_directories(path_.parent_path(), error);
            if (error) {
                return fail(Errc::io, std::format("cannot create log directory '{}': {}",
                                                  path_.parent_path().string(), error.message()));
            }
        }
        file_.open(path_, std::ios::app | std::ios::binary);
        if (!file_) {
            return fail(Errc::io, std::format("cannot open log file '{}'", path_.string()));
        }
        size_ = std::filesystem::exists(path_, error) ? std::filesystem::file_size(path_, error) : 0;
        return ok();
    }

    void write(const Record& record) override {
        nlohmann::json line = nlohmann::json::object();
        line["ts"] = format_time(record.time);
        line["level"] = to_string(record.level);
        line["msg"] = record.message;
        for (const Field& field : record.fields) {
            // Context keys must not overwrite the envelope fields.
            const bool reserved = field.key == "ts" || field.key == "level" || field.key == "msg";
            line[reserved ? "ctx_" + field.key : field.key] = field.value;
        }
        std::string text = line.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        text += '\n';
        if (size_ > 0 && size_ + text.size() > max_bytes_) {
            rotate();
        }
        file_.write(text.data(), static_cast<std::streamsize>(text.size()));
        size_ += text.size();
        if (record.level >= Level::warn) {
            file_.flush();  // problems must reach disk even if the process dies next
        }
    }

    void flush() override { file_.flush(); }

private:
    // file.<n-1> → file.<n>, …, file → file.1; the oldest falls off the end.
    void rotate() {
        file_.close();
        std::error_code ignored;
        auto numbered = [this](unsigned index) {
            return std::filesystem::path{path_.string() + "." + std::to_string(index)};
        };
        std::filesystem::remove(numbered(max_files_), ignored);
        for (unsigned index = max_files_; index > 1; --index) {
            std::filesystem::rename(numbered(index - 1), numbered(index), ignored);
        }
        std::filesystem::rename(path_, numbered(1), ignored);
        file_.open(path_, std::ios::trunc | std::ios::binary);
        size_ = 0;
    }

    std::filesystem::path path_;
    std::uintmax_t max_bytes_;
    unsigned max_files_;
    std::ofstream file_;
    std::uintmax_t size_ = 0;
};

}  // namespace

// --- Levels -----------------------------------------------------------------------------

std::string_view to_string(Level level) noexcept {
    const auto index = static_cast<std::size_t>(level);
    return index < kLevelNames.size() ? kLevelNames[index] : std::string_view{"unknown"};
}

std::optional<Level> level_from_string(std::string_view name) noexcept {
    for (std::size_t index = 0; index < kLevelNames.size(); ++index) {
        if (kLevelNames[index] == name) {
            return static_cast<Level>(index);
        }
    }
    if (name == "warning") {
        return Level::warn;
    }
    return std::nullopt;
}

// --- Sinks ----------------------------------------------------------------------------------

std::unique_ptr<Sink> make_console_sink(bool color) {
    return std::make_unique<ConsoleSink>(color);
}

Result<std::unique_ptr<Sink>> make_jsonl_file_sink(const std::filesystem::path& path,
                                                   std::uintmax_t max_bytes, unsigned max_files) {
    if (max_files == 0) {
        return fail(Errc::invalid_argument, "a rotating log needs max_files >= 1");
    }
    auto sink = std::make_unique<JsonlFileSink>(path, max_bytes, max_files);
    RR_TRY(sink->open());
    return std::unique_ptr<Sink>{std::move(sink)};
}

void MemorySink::write(const Record& record) {
    entries_.push_back(Entry{.level = record.level,
                             .message = std::string{record.message},
                             .fields = std::vector<Field>(record.fields.begin(), record.fields.end())});
}

// --- Configuration -----------------------------------------------------------------------

Result<void> init(const Options& options) {
    std::vector<std::unique_ptr<Sink>> sinks;
    if (options.console) {
        sinks.push_back(make_console_sink(options.color));
    }
    if (!options.jsonl_file.empty()) {
        RR_TRY_ASSIGN(auto file_sink, make_jsonl_file_sink(options.jsonl_file));
        sinks.push_back(std::move(file_sink));
    }
    set_sinks(std::move(sinks));
    set_level(options.level);
    return ok();
}

void set_sinks(std::vector<std::unique_ptr<Sink>> sinks) {
    Logger& state = logger();
    std::vector<std::unique_ptr<Sink>> old;
    {
        const std::lock_guard lock(state.mutex);
        for (auto& sink : state.sinks) {
            sink->flush();
        }
        old.swap(state.sinks);
        state.sinks = std::move(sinks);
    }
    // `old` is destroyed here, outside the lock.
}

void set_level(Level level) noexcept {
    logger().level.store(level, std::memory_order_relaxed);
}

Level level() noexcept {
    return logger().level.load(std::memory_order_relaxed);
}

bool enabled(Level level) noexcept {
    const Level threshold = logger().level.load(std::memory_order_relaxed);
    return level != Level::off && threshold != Level::off && level >= threshold;
}

void flush() {
    Logger& state = logger();
    const std::lock_guard lock(state.mutex);
    for (auto& sink : state.sinks) {
        sink->flush();
    }
}

void write(Level level, std::string_view message) {
    if (!enabled(level)) {
        return;
    }
    const Record record{
        .time = std::chrono::system_clock::now(),
        .level = level,
        .message = message,
        .fields = t_context,
    };
    Logger& state = logger();
    const std::lock_guard lock(state.mutex);
    for (auto& sink : state.sinks) {
        sink->write(record);
    }
}

// --- Context -------------------------------------------------------------------------------

ScopedField::ScopedField(std::string key, std::string value) {
    t_context.push_back(Field{.key = std::move(key), .value = std::move(value)});
}

ScopedField::~ScopedField() {
    if (!t_context.empty()) {
        t_context.pop_back();
    }
}

std::vector<Field> context_snapshot() {
    return t_context;
}

ScopedContext::ScopedContext(std::vector<Field> fields)
    : previous_(std::exchange(t_context, std::move(fields))) {}

ScopedContext::~ScopedContext() {
    t_context = std::move(previous_);
}

}  // namespace rr::log
