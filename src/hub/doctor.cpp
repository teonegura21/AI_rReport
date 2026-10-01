#include <rr/hub/doctor.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <system_error>
#include <utility>

#include <unistd.h>

#include <rr/core/assert.hpp>
#include <rr/core/file_io.hpp>
#include <rr/core/random.hpp>
#include <rr/core/strings.hpp>
#include <rr/library/library.hpp>
#include <rr/runpod/runpod.hpp>
#include <rr/runtime_client/runtime_client.hpp>

namespace rr::hub {

namespace {

// Room for a few videos' audio, frames and intermediate work.
constexpr std::uintmax_t kLowSpace = std::uintmax_t{20} << 30U;

// Every check here asks once more after a transient failure, no more: doctor
// reports the state of things, it does not wait for it to change.
constexpr net::RetryPolicy kPatience{.max_attempts = 2};

std::string gib(std::uintmax_t bytes) {
    return std::format("{:.1f} GiB",
                       static_cast<double>(bytes) / static_cast<double>(std::uintmax_t{1} << 30U));
}

void check_data_directory(const DoctorInputs& inputs, std::vector<Finding>& findings) {
    const std::filesystem::path& directory = inputs.settings->data_dir;
    const std::string check = "data directory";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        findings.push_back({check, Severity::failure,
                            std::format("cannot create '{}': {}", directory.string(), error.message()),
                            "set hub.data_dir to a directory you can write"});
        return;
    }
    const std::filesystem::path probe =
        directory / std::format(".doctor-{}", random_hex(8).value_or("probe"));
    if (auto written = write_file_atomically(probe, "ok"); !written.has_value()) {
        findings.push_back({check, Severity::failure, written.error().message(),
                            std::format("make '{}' writable, or set hub.data_dir", directory.string())});
        return;
    }
    std::filesystem::remove(probe, error);
    std::optional<std::uintmax_t> free;
    if (inputs.free_space) {
        free = inputs.free_space(directory);
    } else if (const auto space = std::filesystem::space(directory, error); !error) {
        free = space.available;
    }
    if (free.has_value() && *free < kLowSpace) {
        findings.push_back(
            {check, Severity::warning, std::format("'{}' has {} free", directory.string(), gib(*free)),
             std::format("videos need room: free some, or set hub.data_dir to a larger disk (at "
                         "least {})",
                         gib(kLowSpace))});
        return;
    }
    findings.push_back({check, Severity::ok,
                        free.has_value() ? std::format("'{}', {} free", directory.string(), gib(*free))
                                         : std::format("'{}'", directory.string()),
                        ""});
}

void check_library(const DoctorInputs& inputs, std::vector<Finding>& findings) {
    const std::string check = "library";
    auto database = library::open_database(inputs.settings->library_database());
    if (!database.has_value()) {
        findings.push_back({check, Severity::failure, database.error().describe(),
                            database.error().code() == Errc::unsupported
                                ? "the library was written by a newer rreport: update this one"
                                : std::format("check '{}'", inputs.settings->library_database().string())});
        return;
    }
    const auto version = database->schema_version();
    findings.push_back({check, Severity::ok,
                        std::format("'{}' (schema version {})", inputs.settings->library,
                                    version.has_value() ? std::to_string(*version) : std::string{"unknown"}),
                        ""});
    if (const auto fts5 = database->supports_fts5(); fts5.has_value() && !*fts5) {
        findings.push_back({"search", Severity::warning,
                            "this SQLite has no FTS5: full-text search will not work",
                            "use a SQLite built with FTS5 (Ubuntu's libsqlite3 is)"});
    }
}

void check_tools(const DoctorInputs& inputs, std::vector<Finding>& findings) {
    struct Tool {
        std::string_view program;
        std::string_view role;
        std::string_view advice;
    };
    static constexpr std::array<Tool, 4> kTools{{
        {.program = "yt-dlp",
         .role = "fetches captions and audio from YouTube",
         .advice = "install it: python3 -m pip install -U yt-dlp (YouTube changes often: keep it current)"},
        {.program = "deno",
         .role = "runs YouTube's JavaScript challenges for yt-dlp",
         .advice = "install it: https://deno.com"},
        {.program = "ffmpeg",
         .role = "extracts audio and frames",
         .advice = "install it: sudo apt install ffmpeg"},
        {.program = "ffprobe", .role = "reads media metadata", .advice = "it comes with ffmpeg"},
    }};
    for (const Tool& tool : kTools) {
        // yt-dlp is the one the configuration names: a name in PATH, or a path.
        const std::string_view program =
            tool.program == "yt-dlp" ? std::string_view{inputs.settings->ytdlp} : tool.program;
        const auto found = inputs.find_program ? inputs.find_program(program) : std::nullopt;
        if (found.has_value()) {
            findings.push_back({std::string{tool.program}, Severity::ok, found->string(), ""});
        } else {
            findings.push_back({std::string{tool.program}, Severity::warning,
                                std::format("not found; capture needs it: it {}", tool.role),
                                std::string{tool.advice}});
        }
    }
}

std::optional<runpod::Pod> check_runpod(const DoctorInputs& inputs, std::vector<Finding>& findings) {
    const std::string check = "RunPod";
    if (!inputs.settings->runpod.has_value()) {
        findings.push_back({check, Severity::warning,
                            "no runpod.pod_id: rreport will not start or stop a pod",
                            "set runpod.pod_id and runpod.api_key to let rreport start and stop your pod"});
        return std::nullopt;
    }
    const RunpodSettings& settings = *inputs.settings->runpod;
    if (settings.api_key.empty()) {
        findings.push_back({check, Severity::failure, "runpod.api_key is not set",
                            "create an API key in RunPod's console (Settings, API Keys) and put it in "
                            "secrets.toml as runpod.api_key, or in RREPORT_RUNPOD__API_KEY"});
        return std::nullopt;
    }
    const runpod::RunpodApi api{
        *inputs.http, {.api_key = settings.api_key, .api_url = settings.api_url}, kPatience};
    auto pod = api.get_pod(settings.pod_id, inputs.stop);
    if (!pod.has_value()) {
        std::string advice = "RunPod's API did not answer: try again in a minute";
        if (pod.error().code() == Errc::permission_denied) {
            advice = "RunPod refused the API key: check runpod.api_key";
        } else if (pod.error().code() == Errc::not_found) {
            advice = std::format("this account has no pod {}: check runpod.pod_id", settings.pod_id);
        }
        findings.push_back({check, Severity::failure, pod.error().message(), advice});
        return std::nullopt;
    }
    findings.push_back(
        {check, Severity::ok,
         std::format("pod {} is {}: {} × {}, ${:.2f}/h", pod->id, pod->status_name, pod->gpu_count,
                     pod->gpu.empty() ? std::string{"GPU"} : pod->gpu, pod->cost_per_hour),
         ""});
    return std::move(*pod);
}

// Where model calls go: the pod's runtime, and nowhere else (ADR-0017), or
// recorded answers in replay mode.
void check_models(const DoctorInputs& inputs, std::vector<Finding>& findings) {
    const HubSettings& settings = *inputs.settings;
    const std::string check = "models";
    if (settings.llm_mode == LlmMode::replay) {
        findings.push_back({check, Severity::ok,
                            std::format("replayed from {}: no model is called", settings.recordings.string()),
                            ""});
    } else if (!settings.runtime.has_value()) {
        findings.push_back({check, Severity::warning, "none can be called: no runtime is configured", ""});
    } else {
        findings.push_back(
            {check, Severity::ok,
             std::format("local only: every call goes to the runtime at {}{}", settings.runtime->url,
                         settings.llm_mode == LlmMode::record ? ", and is recorded" : ""),
             ""});
    }
}

void check_runtime(const DoctorInputs& inputs, const std::optional<runpod::Pod>& pod,
                   std::vector<Finding>& findings) {
    const std::string check = "runtime";
    if (!inputs.settings->runtime.has_value()) {
        findings.push_back({check, Severity::warning, "none configured: models cannot be called",
                            "set runpod.pod_id (the runtime's address follows from it) or runtime.url"});
        return;
    }
    const runtime_client::Endpoint& endpoint = *inputs.settings->runtime;
    if (endpoint.token.empty()) {
        findings.push_back({check, Severity::failure, "runtime.token is not set",
                            "set runtime.token (secrets.toml) or RREPORT_RUNTIME__TOKEN to the pod's "
                            "RR_RUNTIME_TOKEN"});
        return;
    }
    if (pod.has_value() && pod->status != runpod::PodStatus::running) {
        findings.push_back({check, Severity::warning,
                            std::format("not checked: the pod is {}", pod->status_name),
                            "start it: rreport pod start"});
        return;
    }
    const runtime_client::RuntimeClient client{*inputs.http, endpoint, kPatience};
    if (auto ping = client.ping(inputs.stop); !ping.has_value()) {
        findings.push_back(
            {check, Severity::failure,
             std::format("no answer at {}: {}", endpoint.url, ping.error().message()),
             "is the pod running, with the runtime's port exposed over HTTP? rreport pod status"});
        return;
    }
    auto health = client.health(inputs.stop);
    if (!health.has_value()) {
        std::string advice;
        if (health.error().code() == Errc::permission_denied) {
            advice = "the runtime refused the token: runtime.token must equal the pod's RR_RUNTIME_TOKEN";
        } else if (health.error().code() == Errc::unsupported) {
            advice = "run the same rReport release on the hub and the pod";
        }
        findings.push_back({check, Severity::failure, health.error().message(), advice});
        return;
    }
    std::string engines;
    bool all_ready = !health->engines.empty();
    for (const runtime_api::EngineInfo& engine : health->engines) {
        engines += std::format("{}{} ({}) {}", engines.empty() ? "" : ", ", engine.model, engine.role,
                               to_string(engine.state));
        all_ready = all_ready && engine.state == runtime_api::EngineState::ready;
    }
    const std::string detail =
        std::format("{} at {}: profile {}, {}; engines: {}", health->version, endpoint.url, health->profile,
                    describe_gpus(health->gpus), engines);
    if (all_ready) {
        findings.push_back({check, Severity::ok, detail, ""});
    } else {
        findings.push_back({check, Severity::warning, detail,
                            "engines not ready may still be loading their models (minutes for large ones); "
                            "failed ones restart by themselves. Run doctor again, or read the pod's logs"});
    }
    if (health->version != inputs.hub_version) {
        findings.push_back(
            {"versions", Severity::warning,
             std::format("the hub is {} and the runtime {}", inputs.hub_version, health->version),
             "run the same rReport release on the hub and the pod"});
    }
}

}  // namespace

std::string describe_gpus(const std::vector<runtime_api::GpuInfo>& gpus) {
    if (gpus.empty()) {
        return "no GPU";
    }
    std::string text;
    for (const runtime_api::GpuInfo& gpu : gpus) {
        text += std::format("{}{} ({} MiB, {} used)", text.empty() ? "" : "; ", gpu.name,
                            gpu.memory_total_mib, gpu.memory_used_mib);
    }
    return text;
}

std::string_view to_string(Severity severity) noexcept {
    switch (severity) {
        case Severity::ok:
            return "ok";
        case Severity::warning:
            return "warn";
        case Severity::failure:
            return "FAIL";
    }
    return "?";
}

std::vector<Finding> run_doctor(const DoctorInputs& inputs) {
    RR_ASSERT(inputs.settings != nullptr, "doctor needs the hub's settings");
    RR_ASSERT(inputs.http != nullptr, "doctor needs an HTTP client");
    std::vector<Finding> findings;
    check_data_directory(inputs, findings);
    check_library(inputs, findings);
    check_tools(inputs, findings);
    const std::optional<runpod::Pod> pod = check_runpod(inputs, findings);
    check_models(inputs, findings);
    check_runtime(inputs, pod, findings);
    return findings;
}

bool healthy(const std::vector<Finding>& findings) noexcept {
    return std::ranges::none_of(findings,
                                [](const Finding& finding) { return finding.severity == Severity::failure; });
}

std::optional<std::filesystem::path> find_on_path(std::string_view program, std::string_view path_variable) {
    if (program.find('/') != std::string_view::npos) {  // a path, as a shell takes it: not looked up
        std::error_code error;
        std::filesystem::path given{program};
        if (std::filesystem::is_regular_file(given, error) && ::access(given.c_str(), X_OK) == 0) {
            return given;
        }
        return std::nullopt;
    }
    for (const std::string_view directory : split(path_variable, ':')) {
        std::filesystem::path candidate =
            std::filesystem::path{directory.empty() ? std::string_view{"."} : directory} / program;
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && ::access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
    }
    return std::nullopt;
}

}  // namespace rr::hub
