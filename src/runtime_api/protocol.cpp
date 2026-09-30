#include <rr/runtime_api/protocol.hpp>

#include <algorithm>
#include <format>
#include <limits>

namespace rr::runtime_api {

namespace {

// Reads fields of one JSON object strictly: a missing required field or a
// value of the wrong type is an error naming the exact path.
class ObjectReader {
public:
    ObjectReader(const json::Json& object, std::string path) : object_(&object), path_(std::move(path)) {}

    [[nodiscard]] Result<void> expect_object() const {
        if (!object_->is_object()) {
            return fail(Errc::invalid_argument, std::format("health report: '{}' must be an object", path_));
        }
        return ok();
    }

    [[nodiscard]] Result<std::string> string(std::string_view name) const {
        RR_TRY_ASSIGN(const json::Json* value, required(name));
        if (!value->is_string()) {
            return type_error(name, "a string");
        }
        return value->get<std::string>();
    }

    [[nodiscard]] Result<std::int64_t> integer(std::string_view name) const {
        RR_TRY_ASSIGN(const json::Json* value, required(name));
        if (!value->is_number_integer()) {
            return type_error(name, "an integer");
        }
        return value->get<std::int64_t>();
    }

    [[nodiscard]] Result<int> small_integer(std::string_view name) const {
        RR_TRY_ASSIGN(const std::int64_t value, integer(name));
        if (!std::in_range<int>(value)) {
            return type_error(name, "a small integer");
        }
        return static_cast<int>(value);
    }

    // Missing or null: std::nullopt.
    [[nodiscard]] Result<std::optional<std::string>> optional_string(std::string_view name) const {
        const auto found = object_->find(name);
        if (found == object_->end() || found->is_null()) {
            return std::optional<std::string>{};
        }
        if (!found->is_string()) {
            return type_error(name, "a string or null");
        }
        return std::optional<std::string>{found->get<std::string>()};
    }

    [[nodiscard]] Result<std::optional<std::int64_t>> optional_integer(std::string_view name) const {
        const auto found = object_->find(name);
        if (found == object_->end() || found->is_null()) {
            return std::optional<std::int64_t>{};
        }
        if (!found->is_number_integer()) {
            return type_error(name, "an integer or null");
        }
        return std::optional<std::int64_t>{found->get<std::int64_t>()};
    }

    [[nodiscard]] Result<const json::Json*> array(std::string_view name) const {
        RR_TRY_ASSIGN(const json::Json* value, required(name));
        if (!value->is_array()) {
            return type_error(name, "an array");
        }
        return value;
    }

    [[nodiscard]] std::string child_path(std::string_view name, std::size_t index) const {
        return std::format("{}{}{}[{}]", path_, path_.empty() ? "" : ".", name, index);
    }

private:
    [[nodiscard]] Result<const json::Json*> required(std::string_view name) const {
        const auto found = object_->find(name);
        if (found == object_->end()) {
            return fail(Errc::invalid_argument,
                        std::format("health report: '{}' is missing", qualified(name)));
        }
        return &*found;
    }

    [[nodiscard]] std::unexpected<Error> type_error(std::string_view name, std::string_view expected) const {
        return fail(Errc::invalid_argument,
                    std::format("health report: '{}' must be {}", qualified(name), expected));
    }

    [[nodiscard]] std::string qualified(std::string_view name) const {
        return path_.empty() ? std::string{name} : std::format("{}.{}", path_, name);
    }

    const json::Json* object_;  // never null
    std::string path_;
};

json::Json to_json(const GpuInfo& gpu) {
    return json::Json{{"index", gpu.index},
                      {"name", gpu.name},
                      {"memory_total_mib", gpu.memory_total_mib},
                      {"memory_used_mib", gpu.memory_used_mib},
                      {"utilization_percent", gpu.utilization_percent},
                      {"driver_version", gpu.driver_version}};
}

json::Json to_json(const EngineInfo& engine) {
    return json::Json{
        {"model", engine.model},
        {"role", engine.role},
        {"state", std::string{to_string(engine.state)}},
        {"slots", engine.slots},
        {"restarts", engine.restarts},
        {"last_error", engine.last_error.has_value() ? json::Json(*engine.last_error) : json::Json()}};
}

Result<GpuInfo> gpu_from_json(const ObjectReader& reader) {
    RR_TRY(reader.expect_object());
    GpuInfo gpu;
    RR_TRY_ASSIGN(gpu.index, reader.small_integer("index"));
    RR_TRY_ASSIGN(gpu.name, reader.string("name"));
    RR_TRY_ASSIGN(gpu.memory_total_mib, reader.integer("memory_total_mib"));
    RR_TRY_ASSIGN(gpu.memory_used_mib, reader.integer("memory_used_mib"));
    RR_TRY_ASSIGN(gpu.utilization_percent, reader.small_integer("utilization_percent"));
    RR_TRY_ASSIGN(gpu.driver_version, reader.string("driver_version"));
    return gpu;
}

Result<EngineInfo> engine_from_json(const ObjectReader& reader, const std::string& path) {
    RR_TRY(reader.expect_object());
    EngineInfo engine;
    RR_TRY_ASSIGN(engine.model, reader.string("model"));
    RR_TRY_ASSIGN(engine.role, reader.string("role"));
    RR_TRY_ASSIGN(const std::string state, reader.string("state"));
    const auto parsed_state = engine_state_from_string(state);
    if (!parsed_state.has_value()) {
        return fail(Errc::invalid_argument,
                    std::format("health report: '{}.state' has unknown value '{}'", path, state));
    }
    engine.state = *parsed_state;
    RR_TRY_ASSIGN(engine.slots, reader.small_integer("slots"));
    RR_TRY_ASSIGN(engine.restarts, reader.small_integer("restarts"));
    RR_TRY_ASSIGN(engine.last_error, reader.optional_string("last_error"));
    return engine;
}

}  // namespace

std::string blob_path(std::string_view library, std::string_view hash_hex) {
    return std::format("/v1/blobs/{}/{}", library, hash_hex);
}

Result<void> validate_library_name(std::string_view name) {
    const bool valid_characters = std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
    if (name.empty() || name.size() > 64 || !valid_characters || name.front() == '-') {
        return fail(
            Errc::invalid_argument,
            std::format("invalid library name '{}': use 1-64 characters of a-z, 0-9, '_' and '-'", name));
    }
    return ok();
}

std::string_view to_string(EngineState state) noexcept {
    switch (state) {
        case EngineState::starting:
            return "starting";
        case EngineState::ready:
            return "ready";
        case EngineState::failed:
            return "failed";
        case EngineState::stopped:
            return "stopped";
    }
    return "stopped";
}

std::optional<EngineState> engine_state_from_string(std::string_view text) noexcept {
    for (const EngineState state :
         {EngineState::starting, EngineState::ready, EngineState::failed, EngineState::stopped}) {
        if (to_string(state) == text) {
            return state;
        }
    }
    return std::nullopt;
}

json::Json to_json(const HealthReport& report) {
    json::Json gpus = json::Json::array();
    for (const GpuInfo& gpu : report.gpus) {
        gpus.push_back(to_json(gpu));
    }
    json::Json engines = json::Json::array();
    for (const EngineInfo& engine : report.engines) {
        engines.push_back(to_json(engine));
    }
    return json::Json{
        {"protocol", report.protocol},
        {"version", report.version},
        {"profile", report.profile},
        {"gpus", std::move(gpus)},
        {"engines", std::move(engines)},
        {"uptime_s", report.uptime_s},
        {"idle_s", report.idle_s},
        {"stops_in_s", report.stops_in_s.has_value() ? json::Json(*report.stops_in_s) : json::Json()}};
}

Result<HealthReport> health_report_from_json(const json::Json& document) {
    const ObjectReader reader{document, ""};
    RR_TRY(reader.expect_object());
    HealthReport report;
    RR_TRY_ASSIGN(report.protocol, reader.string("protocol"));
    if (report.protocol != kProtocolVersion) {
        return fail(Errc::unsupported,
                    std::format("the runtime speaks protocol v{}, this hub speaks v{}: update both "
                                "to the same release",
                                report.protocol, kProtocolVersion));
    }
    RR_TRY_ASSIGN(report.version, reader.string("version"));
    RR_TRY_ASSIGN(report.profile, reader.string("profile"));
    RR_TRY_ASSIGN(const json::Json* gpus, reader.array("gpus"));
    for (std::size_t index = 0; index < gpus->size(); ++index) {
        RR_TRY_ASSIGN(GpuInfo gpu,
                      gpu_from_json(ObjectReader{(*gpus)[index], reader.child_path("gpus", index)}));
        report.gpus.push_back(std::move(gpu));
    }
    RR_TRY_ASSIGN(const json::Json* engines, reader.array("engines"));
    for (std::size_t index = 0; index < engines->size(); ++index) {
        const std::string path = reader.child_path("engines", index);
        RR_TRY_ASSIGN(EngineInfo engine, engine_from_json(ObjectReader{(*engines)[index], path}, path));
        report.engines.push_back(std::move(engine));
    }
    RR_TRY_ASSIGN(report.uptime_s, reader.integer("uptime_s"));
    RR_TRY_ASSIGN(report.idle_s, reader.integer("idle_s"));
    RR_TRY_ASSIGN(report.stops_in_s, reader.optional_integer("stops_in_s"));
    return report;
}

}  // namespace rr::runtime_api
