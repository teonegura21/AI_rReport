#include <rr/config/config.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include <rr/core/environment.hpp>
#include <rr/core/strings.hpp>

namespace rr::config {
namespace {

constexpr std::array<std::string_view, 5> kLayerNames{"defaults", "file", "secrets_file", "environment",
                                                      "command_line"};

// --- Value helpers ------------------------------------------------------------------

std::string_view type_name(const Value& value) {
    switch (value.index()) {
        case 0:
            return "a string";
        case 1:
            return "an integer";
        case 2:
            return "a number";
        case 3:
            return "a boolean";
        default:
            return "a list of strings";
    }
}

std::string quote(std::string_view text) {
    std::string quoted = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            quoted += '\\';
        }
        quoted += c;
    }
    quoted += '"';
    return quoted;
}

std::string render(const Value& value) {
    return std::visit(
        [](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                return quote(v);
            } else if constexpr (std::is_same_v<T, bool>) {
                return v ? "true" : "false";
            } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
                std::string text = "[";
                for (std::size_t index = 0; index < v.size(); ++index) {
                    text += (index > 0 ? ", " : "") + quote(v[index]);
                }
                return text + "]";
            } else {
                return std::format("{}", v);
            }
        },
        value);
}

// Levenshtein distance, for "did you mean" suggestions on unknown keys.
std::size_t edit_distance(std::string_view a, std::string_view b) {
    std::vector<std::size_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) {
        row[j] = j;
    }
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t above = row[j];
            const std::size_t substitution = diagonal + (a[i - 1] == b[j - 1] ? 0 : 1);
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, substitution});
            diagonal = above;
        }
    }
    return row[b.size()];
}

// Converts a textual value (from the environment or --set) to the type the
// defaults declare for the key.
Result<Value> parse_as(const Value& type_example, std::string_view text) {
    const std::string_view trimmed = trim(text);
    switch (type_example.index()) {
        case 0:
            return Value{std::string{text}};
        case 1: {
            std::int64_t number = 0;
            const auto [end, error] =
                std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), number);
            if (trimmed.empty() || error != std::errc{} || end != trimmed.data() + trimmed.size()) {
                return fail(Errc::invalid_argument, std::format("'{}' is not an integer", text));
            }
            return Value{number};
        }
        case 2: {
            double number = 0;
            const auto [end, error] =
                std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), number);
            if (trimmed.empty() || error != std::errc{} || end != trimmed.data() + trimmed.size()) {
                return fail(Errc::invalid_argument, std::format("'{}' is not a number", text));
            }
            return Value{number};
        }
        case 3: {
            const std::string lower = to_lower_ascii(trimmed);
            if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
                return Value{true};
            }
            if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
                return Value{false};
            }
            return fail(Errc::invalid_argument, std::format("'{}' is not a boolean (use true/false)", text));
        }
        default: {
            std::vector<std::string> items;
            if (!trimmed.empty()) {
                for (const std::string_view item : split(trimmed, ',')) {
                    items.emplace_back(trim(item));
                }
            }
            return Value{std::move(items)};
        }
    }
}

// Accepts a TOML value for a key whose defaults declare `type_example`.
// Integers are accepted where numbers are expected.
Result<Value> coerce(const Value& type_example, Value value) {
    if (value.index() == type_example.index()) {
        return value;
    }
    if (type_example.index() == 2 && value.index() == 1) {
        return Value{static_cast<double>(std::get<std::int64_t>(value))};
    }
    return fail(Errc::invalid_argument,
                std::format("must be {}, got {}", type_name(type_example), type_name(value)));
}

// --- TOML -----------------------------------------------------------------------------

Result<Value> from_toml(const toml::node& node, const std::string& key) {
    if (const auto* s = node.as_string()) {
        return Value{std::string{s->get()}};
    }
    if (const auto* i = node.as_integer()) {
        return Value{i->get()};
    }
    if (const auto* f = node.as_floating_point()) {
        return Value{f->get()};
    }
    if (const auto* b = node.as_boolean()) {
        return Value{b->get()};
    }
    if (const auto* array = node.as_array()) {
        std::vector<std::string> items;
        for (const toml::node& element : *array) {
            const auto* s = element.as_string();
            if (s == nullptr) {
                return fail(Errc::unsupported,
                            std::format("key '{}': only arrays of strings are supported", key));
            }
            items.emplace_back(s->get());
        }
        return Value{std::move(items)};
    }
    return fail(Errc::unsupported, std::format("key '{}': unsupported TOML value type", key));
}

Result<void> flatten(const toml::table& table, const std::string& prefix, std::map<std::string, Value>& out) {
    for (const auto& [raw_key, node] : table) {
        const std::string key =
            prefix.empty() ? std::string{raw_key.str()} : prefix + "." + std::string{raw_key.str()};
        if (const auto* nested = node.as_table()) {
            RR_TRY(flatten(*nested, key, out));
            continue;
        }
        RR_TRY_ASSIGN(Value value, from_toml(node, key));
        out.emplace(key, std::move(value));
    }
    return ok();
}

Result<std::map<std::string, Value>> parse_toml(std::string_view text, const std::string& source) {
    try {
        const toml::table table = toml::parse(text, source);
        std::map<std::string, Value> flat;
        RR_TRY(flatten(table, "", flat));
        return flat;
    } catch (const toml::parse_error& error) {
        const auto& begin = error.source().begin;
        return fail(Errc::invalid_argument,
                    std::format("{}:{}:{}: {}", source, begin.line, begin.column, error.description()));
    }
}

Result<std::string> read_file(const std::filesystem::path& path) {
    const std::ifstream file(path, std::ios::binary);
    if (!file) {
        return fail(Errc::io, std::format("cannot read '{}'", path.string()));
    }
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

}  // namespace

std::string_view to_string(Layer layer) noexcept {
    const auto index = static_cast<std::size_t>(layer);
    return index < kLayerNames.size() ? kLayerNames[index] : std::string_view{"unknown"};
}

bool is_secret_key(std::string_view key) noexcept {
    const auto dot = key.rfind('.');
    const std::string_view last = dot == std::string_view::npos ? key : key.substr(dot + 1);
    constexpr std::array<std::string_view, 4> kMarkers{"token", "api_key", "secret", "password"};
    return std::ranges::any_of(kMarkers, [last](std::string_view marker) {
        return last == marker || last.ends_with(std::string{"_"} + std::string{marker});
    });
}

Result<std::chrono::milliseconds> parse_duration(std::string_view text) {
    const std::string_view trimmed = trim(text);
    std::size_t digits = 0;
    while (digits < trimmed.size() && trimmed[digits] >= '0' && trimmed[digits] <= '9') {
        ++digits;
    }
    const std::string_view number_text = trimmed.substr(0, digits);
    const std::string_view unit = trimmed.substr(digits);
    std::int64_t number = 0;
    const auto [end, error] =
        std::from_chars(number_text.data(), number_text.data() + number_text.size(), number);
    if (digits == 0 || error != std::errc{}) {
        return fail(Errc::invalid_argument,
                    std::format("invalid duration '{}' (examples: 250ms, 30s, 5m, 2h)", text));
    }
    if (unit == "ms") {
        return std::chrono::milliseconds{number};
    }
    if (unit == "s") {
        return std::chrono::milliseconds{number * 1000};
    }
    if (unit == "m") {
        return std::chrono::milliseconds{number * 60 * 1000};
    }
    if (unit == "h") {
        return std::chrono::milliseconds{number * 60 * 60 * 1000};
    }
    return fail(Errc::invalid_argument,
                std::format("invalid duration '{}' (examples: 250ms, 30s, 5m, 2h)", text));
}

// --- Loading ----------------------------------------------------------------------------------

Result<Config> Config::load(const LoadOptions& options) {
    Config config;
    RR_TRY(config.load_defaults(options.defaults_toml));

    if (options.file) {
        std::error_code error;
        if (std::filesystem::exists(*options.file, error)) {
            RR_TRY(config.apply_file(*options.file, Layer::file));
        } else if (options.file_required) {
            return fail(Errc::not_found,
                        std::format("configuration file '{}' does not exist", options.file->string()));
        }
    }
    if (options.secrets_file) {
        RR_TRY(config.apply_secrets_file(*options.secrets_file));
    }
    RR_TRY(config.apply_environment(options.environment ? *options.environment : environment_snapshot(),
                                    options.env_prefix));
    RR_TRY(config.apply_overrides(options.overrides));
    return config;
}

Result<void> Config::load_defaults(std::string_view defaults_toml) {
    // The defaults are the schema: they define which keys exist and their types.
    RR_TRY_ASSIGN(auto defaults, parse_toml(defaults_toml, "<defaults>"));
    for (auto& [key, value] : defaults) {
        const bool secret = is_secret_key(key);
        items_.emplace(key, Item{.value = std::move(value),
                                 .origin = Origin{.layer = Layer::defaults, .detail = ""},
                                 .secret = secret});
    }
    return ok();
}

Result<void> Config::apply_file(const std::filesystem::path& path, Layer layer) {
    RR_TRY_ASSIGN(const std::string text, read_file(path));
    RR_TRY_ASSIGN(auto values, parse_toml(text, path.string()));
    for (auto& [key, value] : values) {
        RR_TRY(
            set(key, std::move(value), Origin{.layer = layer, .detail = path.string()}, /*textual=*/false));
    }
    return ok();
}

Result<void> Config::apply_secrets_file(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return ok();  // secrets are optional
    }
    const auto permissions = std::filesystem::status(path, error).permissions();
    using std::filesystem::perms;
    if (!error && (permissions & (perms::group_all | perms::others_all)) != perms::none) {
        return fail(Errc::permission_denied,
                    std::format("secrets file '{}' is readable by other users; run: chmod 600 '{}'",
                                path.string(), path.string()));
    }
    return apply_file(path, Layer::secrets_file);
}

Result<void> Config::apply_environment(const std::vector<std::pair<std::string, std::string>>& environment,
                                       std::string_view prefix) {
    for (const auto& [name, value] : environment) {
        if (!name.starts_with(prefix)) {
            continue;
        }
        const std::string_view rest = std::string_view{name}.substr(prefix.size());
        if (rest.find("__") == std::string_view::npos) {
            continue;  // application-level variable such as RREPORT_CONFIG
        }
        const std::string key = replace_all(to_lower_ascii(rest), "__", ".");
        RR_TRY(set(key, Value{value}, Origin{.layer = Layer::environment, .detail = name}, /*textual=*/true));
    }
    return ok();
}

Result<void> Config::apply_overrides(const std::vector<std::string>& overrides) {
    for (const std::string& text : overrides) {
        const auto equals = text.find('=');
        if (equals == std::string::npos || equals == 0) {
            return fail(Errc::invalid_argument,
                        std::format("invalid override '{}' (expected key=value)", text));
        }
        const std::string key{trim(std::string_view{text}.substr(0, equals))};
        RR_TRY(set(key, Value{text.substr(equals + 1)}, Origin{.layer = Layer::command_line, .detail = text},
                   /*textual=*/true));
    }
    return ok();
}

Result<void> Config::set(const std::string& key, Value value, Origin origin, bool textual) {
    const auto it = items_.find(key);
    if (it == items_.end()) {
        return fail(Errc::invalid_argument,
                    std::format("unknown configuration key '{}' from {} {}{}", key, to_string(origin.layer),
                                origin.detail, suggest(key)));
    }
    Result<Value> converted = textual ? parse_as(it->second.value, std::get<std::string>(value))
                                      : coerce(it->second.value, std::move(value));
    if (!converted) {
        return fail(Errc::invalid_argument,
                    std::format("configuration key '{}' from {} {}: {}", key, to_string(origin.layer),
                                origin.detail, converted.error().message()));
    }
    it->second.value = std::move(*converted);
    it->second.origin = std::move(origin);
    return ok();
}

std::string Config::suggest(std::string_view unknown_key) const {
    std::string suggestion;
    std::size_t best = 3;  // only suggest close matches
    for (const auto& [known, item] : items_) {
        if (const std::size_t distance = edit_distance(unknown_key, known); distance < best) {
            best = distance;
            suggestion = std::format(" (did you mean '{}'?)", known);
        }
    }
    return suggestion;
}

// --- Access ---------------------------------------------------------------------------------------

Result<const Config::Item*> Config::find(std::string_view key) const {
    const auto it = items_.find(key);
    if (it == items_.end()) {
        return fail(Errc::not_found, std::format("configuration key '{}' is not defined", key));
    }
    return &it->second;
}

namespace {

template <class T>
Result<T> get_as(const auto& item, std::string_view key, std::string_view wanted) {
    if (const T* value = std::get_if<T>(&item->value)) {
        return *value;
    }
    return fail(Errc::invalid_argument,
                std::format("configuration key '{}' is {}, not {}", key, type_name(item->value), wanted));
}

}  // namespace

Result<std::string> Config::get_string(std::string_view key) const {
    RR_TRY_ASSIGN(const Item* item, find(key));
    return get_as<std::string>(item, key, "a string");
}

Result<std::int64_t> Config::get_int(std::string_view key) const {
    RR_TRY_ASSIGN(const Item* item, find(key));
    return get_as<std::int64_t>(item, key, "an integer");
}

Result<double> Config::get_double(std::string_view key) const {
    RR_TRY_ASSIGN(const Item* item, find(key));
    return get_as<double>(item, key, "a number");
}

Result<bool> Config::get_bool(std::string_view key) const {
    RR_TRY_ASSIGN(const Item* item, find(key));
    return get_as<bool>(item, key, "a boolean");
}

Result<std::vector<std::string>> Config::get_string_list(std::string_view key) const {
    RR_TRY_ASSIGN(const Item* item, find(key));
    return get_as<std::vector<std::string>>(item, key, "a list of strings");
}

Result<std::chrono::milliseconds> Config::get_duration(std::string_view key) const {
    RR_TRY_ASSIGN(const std::string text, get_string(key));
    auto duration = parse_duration(text);
    if (!duration) {
        return fail(std::move(duration).error().with_context(std::format("configuration key '{}'", key)));
    }
    return duration;
}

Result<std::filesystem::path> Config::get_path(std::string_view key) const {
    RR_TRY_ASSIGN(const std::string text, get_string(key));
    if (text == "~" || text.starts_with("~/")) {
        const char* home = std::getenv("HOME");  // NOLINT(concurrency-mt-unsafe): read-only at startup
        if (home == nullptr) {
            return fail(Errc::invalid_argument,
                        std::format("configuration key '{}' uses ~ but HOME is not set", key));
        }
        return std::filesystem::path{home} / (text.size() > 2 ? text.substr(2) : "");
    }
    return std::filesystem::path{text};
}

Result<Secret> Config::get_secret(std::string_view key) const {
    RR_TRY_ASSIGN(std::string text, get_string(key));
    return Secret{std::move(text)};
}

bool Config::contains(std::string_view key) const noexcept {
    return items_.contains(key);
}

std::optional<Origin> Config::origin(std::string_view key) const {
    const auto it = items_.find(key);
    if (it == items_.end()) {
        return std::nullopt;
    }
    return it->second.origin;
}

std::vector<Config::Entry> Config::entries() const {
    std::vector<Entry> result;
    result.reserve(items_.size());
    for (const auto& [key, item] : items_) {
        std::string display = render(item.value);
        if (item.secret) {
            const auto* text = std::get_if<std::string>(&item.value);
            display = (text != nullptr && text->empty()) ? "\"\"" : "\"***\"";
        }
        result.push_back(Entry{
            .key = key, .display_value = std::move(display), .origin = item.origin, .secret = item.secret});
    }
    return result;
}

std::string Config::show(bool with_origins) const {
    std::string text;
    for (const Entry& entry : entries()) {
        text += std::format("{} = {}", entry.key, entry.display_value);
        if (with_origins) {
            text += std::format("  # {}{}{}", to_string(entry.origin.layer),
                                entry.origin.detail.empty() ? "" : ": ", entry.origin.detail);
        }
        text += '\n';
    }
    return text;
}

}  // namespace rr::config
