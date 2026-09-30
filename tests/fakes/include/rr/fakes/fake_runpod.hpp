// A stand-in for RunPod's API, in process, for tests: REST v2's pods (read,
// start, stop) and GraphQL's podStop, with the behaviour RunPod documents or
// people report (rr/runpod/runpod.hpp):
//   * statuses move EXITED → STARTING → RUNNING (after `boot_reads` reads of
//     the pod) and back to EXITED on stop;
//   * reads may lag a start: for `start_lag` reads, the pod is still reported
//     with the status it was started from (serve_stale() lags any change);
//   * an action the status does not allow answers 409, in problem details;
//   * the pod-scoped key (kPodKey) is refused by REST (403) and accepted by
//     GraphQL; the account key (kAccountKey) may do everything;
//   * a start can fail for want of GPUs on the pod's machine (400);
//   * requests can be throttled (429 with Retry-After).
#pragma once

#include <algorithm>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <rr/json/json.hpp>
#include <rr/net/http_server.hpp>

namespace rr::fakes {

class FakeRunpod {
public:
    static constexpr std::string_view kAccountKey = "account-key";
    static constexpr std::string_view kPodKey = "pod-key";

    FakeRunpod() {
        using Response = net::HttpServer::Response;
        server_.set_guard([this](const net::HttpServer::Request& request) -> std::optional<Response> {
            {
                const std::lock_guard lock(mutex_);
                if (throttled_ > 0) {
                    --throttled_;
                    Response slow_down =
                        problem(429, "Too Many Requests", "rate limit exceeded for the minute window");
                    slow_down.headers.set("Retry-After", "1");
                    return slow_down;
                }
            }
            const auto auth = request.headers.get("Authorization");
            if (auth != std::format("Bearer {}", kAccountKey) && auth != std::format("Bearer {}", kPodKey)) {
                return problem(401, "Unauthorized", "missing bearer token");
            }
            return std::nullopt;
        });
        server_.route(net::Method::get, "/v2/pods/:id", [this](const net::HttpServer::Request& request) {
            const std::lock_guard lock(mutex_);
            const auto found = pods_.find(std::string{*request.path_param("id")});
            if (found == pods_.end()) {
                return problem(404, "Not Found", "The requested Pod does not exist.");
            }
            Pod& pod = found->second;
            if (pod.stale_reads > 0) {  // RunPod has not caught up with the start yet
                --pod.stale_reads;
                return Response::json(200, json::dump(to_json(found->first, pod.stale_status)));
            }
            if (pod.status == "STARTING" && --pod.reads_until_running <= 0) {
                pod.status = "RUNNING";
            }
            return Response::json(200, json::dump(to_json(found->first, pod.status)));
        });
        server_.route(
            net::Method::post, "/v2/pods/:id/action", [this](const net::HttpServer::Request& request) {
                if (request.headers.get("Authorization") == std::format("Bearer {}", kPodKey)) {
                    return problem(403, "Forbidden", "access denied");
                }
                const auto body = json::parse(request.body);
                const std::string action = body.has_value() ? body->value("action", "") : "";
                const std::lock_guard lock(mutex_);
                ++actions_[action];
                const auto found = pods_.find(std::string{*request.path_param("id")});
                if (found == pods_.end()) {
                    return problem(404, "Not Found", "The requested Pod does not exist.");
                }
                Pod& pod = found->second;
                const std::vector<std::string> allowed = actions_for(pod.status);
                if (std::ranges::find(allowed, action) == allowed.end()) {
                    return problem(409, "Conflict",
                                   std::format("cannot {} a pod that is {}", action, pod.status));
                }
                if (action == "start") {
                    if (no_free_gpus_) {
                        return problem(
                            400, "Bad Request",
                            "There are not enough free GPUs on the host machine to start this pod.");
                    }
                    pod.stale_status = pod.status;
                    pod.stale_reads = start_lag_;
                    pod.status = "STARTING";
                    pod.reads_until_running = boot_reads_;
                } else if (action == "stop") {
                    pod.status = "EXITED";
                    pod.stale_reads = 0;
                }
                return Response::json(200, json::dump(to_json(found->first, pod.status)));
            });
        server_.route(net::Method::post, "/graphql", [this](const net::HttpServer::Request& request) {
            const auto body = json::parse(request.body);
            const std::string query = body.has_value() ? body->value("query", "") : "";
            std::smatch match;
            if (!std::regex_search(query, match,
                                   std::regex{R"re(podStop\(input: \{podId: "([A-Za-z0-9]+)"\}\))re"})) {
                return Response::json(200, R"({"errors":[{"message":"unknown query"}]})");
            }
            const std::lock_guard lock(mutex_);
            ++graphql_stops_;
            const auto found = pods_.find(match[1].str());
            if (found == pods_.end()) {
                return Response::json(200, R"({"data":{"podStop":null}})");
            }
            found->second.status = "EXITED";
            found->second.stale_reads = 0;
            return Response::json(
                200, std::format(R"({{"data":{{"podStop":{{"id":"{}","desiredStatus":"EXITED"}}}}}})",
                                 found->first));
        });
        if (!server_.start().has_value()) {
            throw std::runtime_error("the fake RunPod API cannot listen");
        }
    }

    void add_pod(std::string id, std::string status = "EXITED") {
        const std::lock_guard lock(mutex_);
        pods_[std::move(id)] = Pod{.status = std::move(status)};
    }
    [[nodiscard]] std::string status(std::string_view id) const {
        const std::lock_guard lock(mutex_);
        const auto found = pods_.find(std::string{id});
        return found == pods_.end() ? std::string{} : found->second.status;
    }
    // Reads of a starting pod before it reports RUNNING.
    void set_boot_reads(int reads) {
        const std::lock_guard lock(mutex_);
        boot_reads_ = reads;
    }
    // Reads after a start that still report the status the pod was started
    // from (default 0), before the boot reads begin.
    void set_start_lag(int reads) {
        const std::lock_guard lock(mutex_);
        start_lag_ = reads;
    }
    // The next `reads` reads of pod `id` report `status` instead of its own:
    // a read that has not caught up with someone else's action.
    void serve_stale(std::string_view id, std::string status, int reads) {
        const std::lock_guard lock(mutex_);
        const auto found = pods_.find(id);
        if (found == pods_.end()) {
            throw std::logic_error("serve_stale: no such pod");
        }
        found->second.stale_status = std::move(status);
        found->second.stale_reads = reads;
    }
    // The next `count` requests are answered 429, with Retry-After: 1.
    void throttle_next(int count) {
        const std::lock_guard lock(mutex_);
        throttled_ = count;
    }
    void set_no_free_gpus(bool none) {
        const std::lock_guard lock(mutex_);
        no_free_gpus_ = none;
    }
    // How many times `action` was asked for, allowed or not (requests refused
    // for their key or throttled never reach a pod, so they do not count).
    [[nodiscard]] int actions(std::string_view action) const {
        const std::lock_guard lock(mutex_);
        const auto found = actions_.find(std::string{action});
        return found == actions_.end() ? 0 : found->second;
    }
    [[nodiscard]] int graphql_stops() const {
        const std::lock_guard lock(mutex_);
        return graphql_stops_;
    }

    [[nodiscard]] std::string api_url() const {
        return std::format("http://127.0.0.1:{}/v2", server_.port());
    }
    [[nodiscard]] std::string graphql_url() const {
        return std::format("http://127.0.0.1:{}/graphql", server_.port());
    }

private:
    struct Pod {
        std::string status;
        int reads_until_running = 0;
        std::string stale_status{};  // reported instead of `status` while stale_reads > 0
        int stale_reads = 0;
    };

    static net::HttpServer::Response problem(int status, std::string_view title, std::string_view detail) {
        net::HttpServer::Response response = net::HttpServer::Response::json(
            status, json::dump(json::Json{{"title", title}, {"status", status}, {"detail", detail}}));
        response.content_type = "application/problem+json";
        return response;
    }

    static std::vector<std::string> actions_for(std::string_view status) {
        if (status == "RUNNING") {
            return {"stop", "restart", "terminate"};
        }
        if (status == "EXITED" || status == "ERROR") {
            return {"start", "terminate"};
        }
        if (status == "PROVISIONING" || status == "STARTING") {
            return {"stop", "terminate"};
        }
        return {};
    }

    static json::Json to_json(const std::string& id, const std::string& status) {
        json::Json document{
            {"id", id},
            {"name", "rreport-" + id},
            {"status", status},
            {"actions", actions_for(status)},
            {"gpu", {{"id", "NVIDIA GeForce RTX 4090"}, {"count", 1}, {"vcpuCount", 16}, {"memory", 64}}},
            {"cost", status == "EXITED" ? 0.0 : 0.44},
            {"dataCenterId", "EU-RO-1"},
            {"runtime", nullptr}};
        if (status == "RUNNING") {
            document["runtime"] = {{"uptime", 42}, {"ports", json::Json::array()}};
        }
        return document;
    }

    mutable std::mutex mutex_;
    std::map<std::string, Pod, std::less<>> pods_;
    std::map<std::string, int, std::less<>> actions_;
    int graphql_stops_ = 0;
    int boot_reads_ = 1;
    int start_lag_ = 0;
    int throttled_ = 0;
    bool no_free_gpus_ = false;
    net::HttpServer server_{net::HttpServer::Options{.host = "127.0.0.1", .port = 0}};
};

}  // namespace rr::fakes
