// rr/config/config.hpp — layered configuration with provenance and secrets.
//
// Why this exists
//   Both programs need configuration from several places. Each layer
//   overrides the ones before it:
//
//     defaults (built into the binary)
//       < config file (~/.config/rreport/config.toml)
//       < secrets file (owner-readable only)
//       < environment variables (RREPORT_RUNTIME__URL=… → runtime.url)
//       < command-line overrides (--set runtime.url=…)
//
//   Two properties make this safe to operate:
//
//   1. The defaults are the schema. Every settable key appears in the
//      built-in defaults with its type. A typo ("runtime.ulr") or a wrong
//      type fails loading with a precise message and a "did you mean"
//      suggestion, instead of being silently ignored.
//   2. Every value remembers its origin. `rreport config show --origin`
//      answers "why is this value what it is?" in one command.
//
//   Keys whose last segment is or ends with "token", "api_key", "secret" or
//   "password" are secrets: masked in listings and read through get_secret().
//
// Environment mapping
//   RREPORT_<SECTION>__<NAME> maps to "<section>.<name>" (lowercased, "__"
//   becomes "."). Variables with the prefix but without "__" (e.g.
//   RREPORT_CONFIG) are left to the application.
//
// Thread-safety: a loaded Config is immutable and safe to share.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/core/secret.hpp>

namespace rr::config {

enum class Layer : std::uint8_t {
    defaults,
    file,
    secrets_file,
    environment,
    command_line
};

[[nodiscard]] std::string_view to_string(Layer layer) noexcept;

// Where a value came from: the layer, plus the file path, variable name or argument.
struct Origin {
    Layer layer = Layer::defaults;
    std::string detail;

    friend bool operator==(const Origin&, const Origin&) = default;
};

// A configuration value: a TOML scalar or an array of strings.
using Value = std::variant<std::string, std::int64_t, double, bool, std::vector<std::string>>;

struct LoadOptions {
    // Built-in defaults, as TOML text. They are also the schema: the set of keys and their types.
    std::string_view defaults_toml{};

    // Optional user configuration file. If `file_required` is false, a missing
    // file is skipped silently (the usual case for the default location).
    std::optional<std::filesystem::path> file{};
    bool file_required = false;

    // Optional secrets file (API keys, tokens). On POSIX it must not be
    // readable by group or others (chmod 600).
    std::optional<std::filesystem::path> secrets_file{};

    std::string env_prefix = "RREPORT_";

    // The environment to read. nullopt means the process environment; tests
    // pass an explicit list.
    std::optional<std::vector<std::pair<std::string, std::string>>> environment{};

    // "key=value" command-line overrides, applied last.
    std::vector<std::string> overrides{};
};

class Config {
public:
    [[nodiscard]] static Result<Config> load(const LoadOptions& options);

    [[nodiscard]] Result<std::string> get_string(std::string_view key) const;
    [[nodiscard]] Result<std::int64_t> get_int(std::string_view key) const;
    [[nodiscard]] Result<double> get_double(std::string_view key) const;
    [[nodiscard]] Result<bool> get_bool(std::string_view key) const;
    [[nodiscard]] Result<std::vector<std::string>> get_string_list(std::string_view key) const;

    // A duration written with a unit: "250ms", "30s", "5m", "2h".
    [[nodiscard]] Result<std::chrono::milliseconds> get_duration(std::string_view key) const;

    // A path; a leading "~/" expands to $HOME.
    [[nodiscard]] Result<std::filesystem::path> get_path(std::string_view key) const;

    // Any string key, wrapped as a Secret (intended for secret keys).
    [[nodiscard]] Result<Secret> get_secret(std::string_view key) const;

    [[nodiscard]] bool contains(std::string_view key) const noexcept;
    [[nodiscard]] std::optional<Origin> origin(std::string_view key) const;

    struct Entry {
        std::string key;
        std::string display_value;  // TOML-style; "***" for non-empty secrets
        Origin origin;
        bool secret = false;
    };

    // Every key, sorted.
    [[nodiscard]] std::vector<Entry> entries() const;

    // One "key = value" line per key; with_origins appends "# layer: detail".
    [[nodiscard]] std::string show(bool with_origins) const;

private:
    struct Item {
        Value value;
        Origin origin;
        bool secret = false;
    };

    [[nodiscard]] Result<const Item*> find(std::string_view key) const;

    // Loading steps, one per layer (see load()).
    [[nodiscard]] Result<void> load_defaults(std::string_view defaults_toml);
    [[nodiscard]] Result<void> apply_file(const std::filesystem::path& path, Layer layer);
    [[nodiscard]] Result<void> apply_secrets_file(const std::filesystem::path& path);
    [[nodiscard]] Result<void> apply_environment(
        const std::vector<std::pair<std::string, std::string>>& environment, std::string_view prefix);
    [[nodiscard]] Result<void> apply_overrides(const std::vector<std::string>& overrides);

    // Sets a known key. `textual` values (environment, --set) are parsed into
    // the key's type; TOML values are type-checked.
    [[nodiscard]] Result<void> set(const std::string& key, Value value, Origin origin, bool textual);
    [[nodiscard]] std::string suggest(std::string_view unknown_key) const;

    std::map<std::string, Item, std::less<>> items_;
};

[[nodiscard]] bool is_secret_key(std::string_view key) noexcept;

// Parses "250ms" / "30s" / "5m" / "2h" (a non-negative integer and a unit).
[[nodiscard]] Result<std::chrono::milliseconds> parse_duration(std::string_view text);

}  // namespace rr::config
