#include <rr/runpod/runpod.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <type_traits>
#include <utility>

#include <rr/core/strings.hpp>
#include <rr/json/json.hpp>
#include <rr/log/log.hpp>

namespace rr::runpod {

namespace {

using namespace std::chrono_literals;

constexpr auto kRequestTimeout = 30s;

struct StatusName {
    PodStatus status;
    std::string_view name;
};

constexpr std::array<StatusName, 6> kStatusNames{{
    {.status = PodStatus::provisioning, .name = "PROVISIONING"},
    {.status = PodStatus::starting, .name = "STARTING"},
    {.status = PodStatus::running, .name = "RUNNING"},
    {.status = PodStatus::exited, .name = "EXITED"},
    {.status = PodStatus::error, .name = "ERROR"},
    {.status = PodStatus::terminated, .name = "TERMINATED"},
}};

// RunPod's explanation of a failure: RFC 9457 problem details ("detail",
// "title"), or the "message"/"error" of older endpoints; else the body's start.
std::string explanation(std::string_view body) {
    if (auto document = json::parse(body); document.has_value() && document->is_object()) {
        for (const char* field : {"detail", "message", "error", "title"}) {
            const auto found = document->find(field);
            if (found != document->end() && found->is_string() && !found->get<std::string>().empty()) {
                return found->get<std::string>();
            }
        }
    }
    return std::string{trim(body).substr(0, 300)};
}

// The error for a non-2xx answer (the table is in the header comment).
Error error_from(const net::HttpClient::Response& response, std::string_view doing) {
    const std::string detail = explanation(response.body);
    Error error{net::errc_for_http_status(response.status),
                std::format("RunPod could not {} (HTTP {}): {}", doing, response.status, detail)};
    if (response.status == 400 && detail.find("not enough free GPUs") != std::string::npos) {
        error = Error{
            Errc::unavailable,
            std::format("RunPod could not {}: {} A stopped pod resumes on its own machine only, and that "
                        "machine has no free GPU now: try again later, or create a new pod on the same "
                        "network volume",
                        doing, detail)};
    }
    if (const auto retry_after = response.headers.get("Retry-After"); retry_after.has_value()) {
        if (const auto delay = net::parse_retry_after(*retry_after); delay.has_value()) {
            error = std::move(error).with_retry_after(*delay);
        }
    }
    return error;
}

template <class T>
T value_or(const json::Json& object, const char* field, T fallback) {
    const auto found = object.find(field);
    if (found == object.end()) {
        return fallback;
    }
    if constexpr (std::is_same_v<T, std::string>) {
        return found->is_string() ? found->get<std::string>() : fallback;
    } else {
        return found->is_number() ? found->get<T>() : fallback;
    }
}

Result<Pod> pod_from_json(const json::Json& document) {
    if (!document.is_object()) {
        return fail(Errc::invalid_argument, "RunPod answered with something that is not a pod");
    }
    Pod pod;
    RR_TRY_ASSIGN(pod.id, json::get_string(document, "id"));
    RR_TRY_ASSIGN(pod.status_name, json::get_string(document, "status"));
    pod.status = pod_status_from_string(pod.status_name);
    pod.name = value_or<std::string>(document, "name", "");
    if (const auto actions = document.find("actions"); actions != document.end() && actions->is_array()) {
        for (const json::Json& action : *actions) {
            if (action.is_string()) {
                pod.actions.push_back(action.get<std::string>());
            }
        }
    }
    if (const auto gpu = document.find("gpu"); gpu != document.end() && gpu->is_object()) {
        pod.gpu = value_or<std::string>(*gpu, "id", "");
        pod.gpu_count = value_or<int>(*gpu, "count", 0);
    }
    pod.cost_per_hour = value_or<double>(document, "cost", 0.0);
    if (const auto runtime = document.find("runtime"); runtime != document.end() && runtime->is_object()) {
        if (const auto uptime = runtime->find("uptime");
            uptime != runtime->end() && uptime->is_number_integer()) {
            pod.uptime_s = uptime->get<std::int64_t>();
        }
    }
    pod.data_center = value_or<std::string>(document, "dataCenterId", "");
    return pod;
}

}  // namespace

std::string_view to_string(PodStatus status) noexcept {
    for (const StatusName& entry : kStatusNames) {
        if (entry.status == status) {
            return entry.name;
        }
    }
    return "UNKNOWN";
}

PodStatus pod_status_from_string(std::string_view name) noexcept {
    for (const StatusName& entry : kStatusNames) {
        if (entry.name == name) {
            return entry.status;
        }
    }
    return PodStatus::unknown;
}

std::string proxy_url(std::string_view pod_id, int port) {
    return std::format("https://{}-{}.proxy.runpod.net", pod_id, port);
}

Result<void> validate_pod_id(std::string_view pod_id) {
    const bool valid = !pod_id.empty() && pod_id.size() <= 64 && std::ranges::all_of(pod_id, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    });
    if (!valid) {
        return fail(Errc::invalid_argument,
                    std::format("'{}' is not a RunPod pod id (letters and digits, at most 64)", pod_id));
    }
    return ok();
}

RunpodApi::RunpodApi(const net::HttpClient& http, Options options, net::RetryPolicy retry)
    : http_(&http)
    , options_(std::move(options))
    , retry_(retry) {
    while (options_.api_url.ends_with('/')) {
        options_.api_url.pop_back();
    }
}

net::HttpClient::Request RunpodApi::request(net::Method method, std::string url) const {
    net::HttpClient::Request request{.method = method, .url = std::move(url), .timeout = kRequestTimeout};
    request.headers.set("Authorization", std::format("Bearer {}", options_.api_key.expose()));
    request.headers.set("Accept", "application/json");
    return request;
}

Result<Pod> RunpodApi::get_pod(std::string_view pod_id, const std::stop_token& stop) const {
    RR_TRY(validate_pod_id(pod_id));
    const net::HttpClient::Request get =
        request(net::Method::get, std::format("{}/pods/{}", options_.api_url, pod_id));
    return net::with_retries(
        retry_,
        [&](int) -> Result<Pod> {
            RR_TRY_ASSIGN(const net::HttpClient::Response response, http_->send(get, stop));
            if (!response.success()) {
                return fail(error_from(response, std::format("read pod {}", pod_id)));
            }
            RR_TRY_ASSIGN(const json::Json document, json::parse(response.body));
            return pod_from_json(document);
        },
        stop);
}

Result<Pod> RunpodApi::act(std::string_view pod_id, std::string_view action,
                           const std::stop_token& stop) const {
    RR_TRY(validate_pod_id(pod_id));
    net::HttpClient::Request post =
        request(net::Method::post, std::format("{}/pods/{}/action", options_.api_url, pod_id));
    post.headers.set("Content-Type", "application/json");
    post.body = json::dump(json::Json{{"action", action}});
    return net::with_retries(
        retry_,
        [&](int) -> Result<Pod> {
            RR_TRY_ASSIGN(const net::HttpClient::Response response, http_->send(post, stop));
            if (!response.success()) {
                return fail(error_from(response, std::format("{} pod {}", action, pod_id)));
            }
            RR_TRY_ASSIGN(const json::Json document, json::parse(response.body));
            return pod_from_json(document);
        },
        stop);
}

Result<Pod> RunpodApi::start_pod(std::string_view pod_id, const std::stop_token& stop) const {
    return act(pod_id, "start", stop);
}

Result<Pod> RunpodApi::stop_pod(std::string_view pod_id, const std::stop_token& stop) const {
    return act(pod_id, "stop", stop);
}

Result<void> RunpodApi::stop_with_graphql(std::string_view pod_id, const std::stop_token& stop) const {
    RR_TRY(validate_pod_id(pod_id));
    net::HttpClient::Request post = request(net::Method::post, options_.graphql_url);
    post.headers.set("Content-Type", "application/json");
    // validate_pod_id() leaves nothing that needs escaping inside the query's string literal.
    post.body = json::dump(json::Json{
        {"query",
         std::format(R"(mutation {{ podStop(input: {{podId: "{}"}}) {{ id desiredStatus }} }})", pod_id)}});
    return net::with_retries(
        retry_,
        [&](int) -> Result<void> {
            RR_TRY_ASSIGN(const net::HttpClient::Response response, http_->send(post, stop));
            if (!response.success()) {
                return fail(error_from(response, std::format("stop pod {} through GraphQL", pod_id)));
            }
            RR_TRY_ASSIGN(const json::Json document, json::parse(response.body));
            // GraphQL reports failures in "errors", with HTTP 200.
            if (const auto errors = document.find("errors");
                errors != document.end() && errors->is_array() && !errors->empty()) {
                const json::Json& first = errors->front();
                const std::string message = first.is_object()
                                                ? value_or<std::string>(first, "message", json::dump(first))
                                                : json::dump(first);
                const std::string lower = to_lower_ascii(message);
                const bool refused = lower.find("authoriz") != std::string::npos ||
                                     lower.find("permission") != std::string::npos ||
                                     lower.find("forbidden") != std::string::npos;
                return fail(refused ? Errc::permission_denied : Errc::unavailable,
                            std::format("RunPod could not stop pod {} through GraphQL: {}", pod_id, message));
            }
            const auto stopped = json::find(document, "data.podStop");
            if (!stopped.has_value() || (*stopped)->is_null()) {
                return fail(Errc::not_found, std::format("RunPod's GraphQL API knows no pod {}", pod_id));
            }
            return ok();
        },
        stop);
}

Result<void> RunpodApi::stop_self(std::string_view pod_id, const std::stop_token& stop) const {
    auto stopped = stop_pod(pod_id, stop);
    if (stopped.has_value()) {
        return ok();
    }
    if (stopped.error().code() != Errc::permission_denied) {
        return fail(std::move(stopped).error());
    }
    log::warn("RunPod's REST API refused this pod's key ({}); stopping through its GraphQL API instead",
              stopped.error().message());
    return with_context(stop_with_graphql(pod_id, stop), "after the REST API refused the key");
}

}  // namespace rr::runpod
