#include <rr/runtime_client/runtime_client.hpp>

#include <algorithm>
#include <format>
#include <utility>

#include <rr/json/json.hpp>
#include <rr/llm/openai_provider.hpp>

namespace rr::runtime_client {

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// Everything but uploads is small: an answer that takes longer is lost.
constexpr auto kRequestTimeout = 15s;
// An upload may take long; one that stops moving has failed.
constexpr auto kUploadIdleTimeout = 60s;

// "commander ready, infantry starting (restarts: 2; last error: …)".
std::string describe_engines(const runtime_api::HealthReport& report) {
    std::string text;
    for (const runtime_api::EngineInfo& engine : report.engines) {
        if (!text.empty()) {
            text += ", ";
        }
        text += std::format("{} {}", engine.model, to_string(engine.state));
        if (engine.restarts > 0 || engine.last_error.has_value()) {
            text += std::format(" (restarts: {}; last error: {})", engine.restarts,
                                engine.last_error.value_or("none"));
        }
    }
    return text.empty() ? std::string{"no engines"} : text;
}

bool all_ready(const runtime_api::HealthReport& report) {
    return !report.engines.empty() &&
           std::ranges::all_of(report.engines, [](const runtime_api::EngineInfo& engine) {
               return engine.state == runtime_api::EngineState::ready;
           });
}

// Errors that waiting can end: the pod or an engine is (re)starting, or the
// proxy has no route to a pod that is still booting (its 404 page).
bool worth_waiting(const Error& error) {
    return error.transient() || error.code() == Errc::not_found;
}

}  // namespace

RuntimeClient::RuntimeClient(const net::HttpClient& http, Endpoint endpoint, net::RetryPolicy retry)
    : http_(&http)
    , endpoint_(std::move(endpoint))
    , retry_(retry) {
    while (endpoint_.url.ends_with('/')) {
        endpoint_.url.pop_back();
    }
}

net::HttpClient::Request RuntimeClient::request(net::Method method, std::string_view path) const {
    net::HttpClient::Request request{
        .method = method, .url = endpoint_.url + std::string{path}, .timeout = kRequestTimeout};
    request.headers.set("Authorization", std::format("Bearer {}", endpoint_.token.expose()));
    return request;
}

Result<net::HttpClient::Response> RuntimeClient::send(const net::HttpClient::Request& request,
                                                      const std::stop_token& stop) const {
    return net::with_retries(
        retry_, [&](int) { return net::expect_success(http_->send(request, stop)); }, stop);
}

Result<void> RuntimeClient::ping(const std::stop_token& stop) const {
    // The one open endpoint: no token is sent where none is needed.
    const net::HttpClient::Request ping{.url = endpoint_.url + std::string{runtime_api::kPathPing},
                                        .timeout = kRequestTimeout};
    RR_TRY_ASSIGN(const net::HttpClient::Response response, send(ping, stop));
    if (response.body != "ok") {
        return fail(Errc::unsupported,
                    std::format("something answers at {}, but it is not an rReport runtime", endpoint_.url));
    }
    return ok();
}

Result<runtime_api::HealthReport> RuntimeClient::health(const std::stop_token& stop) const {
    RR_TRY_ASSIGN(const net::HttpClient::Response response,
                  send(request(net::Method::get, runtime_api::kPathHealth), stop));
    RR_TRY_ASSIGN(const json::Json document, json::parse(response.body));
    // Refuses a runtime of another protocol version (Errc::unsupported).
    return runtime_api::health_report_from_json(document);
}

Result<void> RuntimeClient::heartbeat(const std::stop_token& stop) const {
    RR_TRY(send(request(net::Method::post, runtime_api::kPathHeartbeat), stop));
    return ok();
}

Result<std::vector<ModelInfo>> RuntimeClient::models(const std::stop_token& stop) const {
    RR_TRY_ASSIGN(const net::HttpClient::Response response,
                  send(request(net::Method::get, runtime_api::kPathModels), stop));
    RR_TRY_ASSIGN(const json::Json document, json::parse(response.body));
    RR_TRY_ASSIGN(const json::Json* data, json::find(document, "data"));
    if (!data->is_array()) {
        return fail(Errc::invalid_argument, "the model list's \"data\" is not an array");
    }
    std::vector<ModelInfo> models;
    for (const json::Json& entry : *data) {
        ModelInfo model;
        RR_TRY_ASSIGN(model.id, json::get_string(entry, "id"));
        RR_TRY_ASSIGN(model.role, json::get_string(entry, "role"));
        RR_TRY_ASSIGN(model.state, json::get_string(entry, "state"));
        models.push_back(std::move(model));
    }
    return models;
}

Result<bool> RuntimeClient::has_blob(std::string_view library, const Blake3Digest& hash,
                                     const std::stop_token& stop) const {
    RR_TRY(runtime_api::validate_library_name(library));
    const net::HttpClient::Request head =
        request(net::Method::head, runtime_api::blob_path(library, hash.hex()));
    return net::with_retries(
        retry_,
        [&](int) -> Result<bool> {
            RR_TRY_ASSIGN(const net::HttpClient::Response response, http_->send(head, stop));
            if (response.status == 404) {
                return false;
            }
            RR_TRY(net::expect_success(response));
            return true;
        },
        stop);
}

Result<void> RuntimeClient::put_blob(std::string_view library, const Blake3Digest& hash,
                                     const std::filesystem::path& file, const std::stop_token& stop) const {
    RR_TRY(runtime_api::validate_library_name(library));
    net::HttpClient::Request upload = request(net::Method::put, runtime_api::blob_path(library, hash.hex()));
    upload.body_file = file;
    upload.timeout.reset();  // as long as it keeps moving
    upload.idle_timeout = kUploadIdleTimeout;
    upload.headers.set("Content-Type", "application/octet-stream");
    RR_TRY(send(upload, stop));
    return ok();
}

Result<runtime_api::HealthReport> RuntimeClient::wait_until_ready(
    std::chrono::milliseconds timeout, const std::stop_token& stop,
    std::chrono::milliseconds poll_every) const {
    const auto deadline = Clock::now() + timeout;
    // Each poll is one attempt: the loop itself is the retry.
    const RuntimeClient once{*http_, endpoint_, net::RetryPolicy::none()};
    std::string last_seen = "no answer yet";
    while (true) {
        auto report = once.health(stop);
        if (report.has_value()) {
            if (all_ready(*report)) {
                return report;
            }
            last_seen = describe_engines(*report);
        } else if (report.error().code() == Errc::cancelled || !worth_waiting(report.error())) {
            return fail(std::move(report).error());
        } else {
            last_seen = report.error().describe();
        }
        if (Clock::now() >= deadline) {
            return fail(
                Errc::timeout,
                std::format("the runtime at {} was not ready within {} s; last seen: {}", endpoint_.url,
                            std::chrono::duration_cast<std::chrono::seconds>(timeout).count(), last_seen));
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (!net::pause_for(std::clamp(remaining, std::chrono::milliseconds{0}, poll_every), stop)) {
            return fail(Errc::cancelled, "stopped while waiting for the runtime");
        }
    }
}

std::unique_ptr<llm::LlmProvider> runtime_provider(const net::HttpClient& http, const Endpoint& endpoint,
                                                   CallLimits limits) {
    return std::make_unique<llm::OpenAiProvider>(http,
                                                 llm::OpenAiOptions{.name = std::string{kRuntimeProviderName},
                                                                    .base_url = endpoint.url,
                                                                    .api_key = endpoint.token,
                                                                    .dialect = llm::Dialect::llama_server,
                                                                    .caps = kRuntimeProviderCaps,
                                                                    .idle_timeout = limits.idle_timeout,
                                                                    .call_timeout = limits.call_timeout});
}

}  // namespace rr::runtime_client
