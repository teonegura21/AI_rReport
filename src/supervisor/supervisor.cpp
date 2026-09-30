#include <rr/supervisor/supervisor.hpp>

#include <atomic>
#include <condition_variable>
#include <format>
#include <map>
#include <mutex>
#include <stop_token>
#include <thread>
#include <unordered_map>

#include <rr/blob/blob_store.hpp>
#include <rr/core/strings.hpp>
#include <rr/json/json.hpp>
#include <rr/log/log.hpp>
#include <rr/net/http_client.hpp>
#include <rr/net/http_server.hpp>
#include <rr/proc/process.hpp>
#include <rr/supervisor/engine.hpp>
#include <rr/supervisor/gpu.hpp>
#include <rr/supervisor/proxy.hpp>
#include <rr/supervisor/watchdog.hpp>

namespace rr::supervisor {

using net::HttpServer;
using runtime_api::EngineState;
using Response = HttpServer::Response;

namespace {

using namespace std::chrono_literals;

constexpr auto kGpuCacheTtl = 5s;
constexpr auto kGarbageCollectionInterval = 1h;
constexpr auto kStopCommandRetry = 5min;
constexpr std::string_view kBearerPrefix = "Bearer ";

std::int64_t whole_seconds(std::chrono::milliseconds duration) {
    return std::chrono::duration_cast<std::chrono::seconds>(duration).count();
}

Response unauthorized() {
    Response denied = Response::error(Error{Errc::permission_denied, "missing or invalid bearer token"});
    denied.status = 401;  // not authenticated (403 would mean: known, but not allowed)
    denied.headers.add("WWW-Authenticate", R"(Bearer realm="rreport-runtime")");
    return denied;
}

}  // namespace

struct Supervisor::Impl {
    SupervisorSettings settings;
    const Clock& clock;
    std::chrono::steady_clock::time_point started;
    Watchdog watchdog;
    net::HttpClient engine_client{net::HttpClient::Options{.connect_timeout = 5s}};
    std::vector<std::unique_ptr<Engine>> engines;
    std::unordered_map<std::string, Engine*> engine_for_model;

    std::mutex stores_mutex;
    std::map<std::string, blob::BlobStore, std::less<>> stores;  // one per library, opened on demand

    mutable std::mutex gpu_mutex;
    mutable std::optional<std::chrono::steady_clock::time_point> gpu_checked;
    mutable std::vector<runtime_api::GpuInfo> gpus;

    std::atomic<bool> stop_decided{false};
    std::optional<std::chrono::steady_clock::time_point> stop_command_attempted;
    std::stop_source shutdown;  // requested first by stop(): relays give up within a tick

    std::unique_ptr<HttpServer> server;
    std::mutex watch_mutex;
    std::condition_variable_any watch_wake;
    std::jthread watcher;  // declared last: joined before the members above go away

    Impl(SupervisorSettings value, const Clock& chosen_clock)
        : settings(std::move(value))
        , clock(chosen_clock)
        , started(chosen_clock.steady_now())
        , watchdog(chosen_clock, settings.idle_stop) {}

    // --- Health ---------------------------------------------------------------
    std::vector<runtime_api::GpuInfo> current_gpus() const {
        const std::lock_guard lock(gpu_mutex);
        const auto now = clock.steady_now();
        if (!gpu_checked.has_value() || now - *gpu_checked >= kGpuCacheTtl) {
            gpu_checked = now;
            auto queried = settings.query_gpus ? settings.query_gpus() : query_gpus();
            gpus = queried.has_value() ? std::move(*queried) : std::vector<runtime_api::GpuInfo>{};
        }
        return gpus;
    }

    runtime_api::HealthReport health() const {
        runtime_api::HealthReport report;
        report.version = settings.version;
        report.profile = settings.profile;
        report.gpus = current_gpus();
        for (const auto& engine : engines) {
            report.engines.push_back(engine->info());
        }
        report.uptime_s =
            std::chrono::duration_cast<std::chrono::seconds>(clock.steady_now() - started).count();
        report.idle_s = whole_seconds(watchdog.idle_for());
        if (const auto remaining = watchdog.stops_in(); remaining.has_value()) {
            report.stops_in_s = whole_seconds(*remaining);
        }
        return report;
    }

    // --- Authentication ---------------------------------------------------------
    std::optional<Response> guard(const HttpServer::Request& request) const {
        if (request.path == runtime_api::kPathPing) {
            return std::nullopt;
        }
        const std::string_view header = request.headers.get("Authorization").value_or("");
        const bool bearer = header.size() > kBearerPrefix.size() &&
                            iequals_ascii(header.substr(0, kBearerPrefix.size()), kBearerPrefix);
        if (bearer && settings.token.matches(header.substr(kBearerPrefix.size()))) {
            return std::nullopt;
        }
        return unauthorized();
    }

    // --- Model calls ---------------------------------------------------------------
    Response proxy_to_engine(const HttpServer::Request& request, std::string_view path) {
        Watchdog::InFlight in_flight = watchdog.begin_request();
        const auto body = json::parse(request.body);
        if (!body.has_value() || !body->is_object()) {
            return Response::error(Error{Errc::invalid_argument, "the request body must be a JSON object"});
        }
        const auto model = body->find("model");
        if (model == body->end() || !model->is_string()) {
            return Response::error(Error{Errc::invalid_argument, R"(the request needs a "model")"});
        }
        bool streamed = false;
        if (const auto stream = body->find("stream"); stream != body->end()) {
            if (!stream->is_boolean()) {
                return Response::error(Error{Errc::invalid_argument, R"("stream" must be true or false)"});
            }
            streamed = stream->get<bool>();
        }
        const auto engine = engine_for_model.find(model->get<std::string>());
        if (engine == engine_for_model.end()) {
            return Response::error(
                Error{Errc::not_found, std::format("no engine serves model '{}' in profile '{}'",
                                                   model->get<std::string>(), settings.profile)});
        }
        if (!engine->second->ready()) {
            const runtime_api::EngineInfo info = engine->second->info();
            Error error{Errc::unavailable,
                        std::format("model '{}' is {}{}", info.model, to_string(info.state),
                                    info.last_error.has_value() ? ": " + *info.last_error : "")};
            if (info.state == EngineState::starting) {
                error = std::move(error).with_retry_after(10s);
            }
            return Response::error(error);
        }
        return proxy(engine_client,
                     ProxyOptions{.url = engine->second->base_url() + std::string{path},
                                  .idle_timeout = settings.proxy_idle_timeout,
                                  .keep_alive = streamed ? settings.stream_keep_alive : std::nullopt,
                                  .head_patience = settings.stream_head_patience,
                                  .shutdown = shutdown.get_token()},
                     request, std::move(in_flight));
    }

    Response list_models() const {
        json::Json data = json::Json::array();
        for (const auto& engine : engines) {
            const runtime_api::EngineInfo info = engine->info();
            data.push_back(json::Json{{"id", info.model},
                                      {"object", "model"},
                                      {"owned_by", "rreport"},
                                      {"role", info.role},
                                      {"state", std::string{to_string(info.state)}}});
        }
        return Response::json(200, json::dump(json::Json{{"object", "list"}, {"data", std::move(data)}}));
    }

    // --- Blobs ------------------------------------------------------------------------
    Result<blob::BlobStore> store_for(std::string_view library) {
        RR_TRY(runtime_api::validate_library_name(library));
        const std::lock_guard lock(stores_mutex);
        if (const auto found = stores.find(library); found != stores.end()) {
            return found->second;
        }
        RR_TRY_ASSIGN(blob::BlobStore store, blob::BlobStore::open(settings.blobs_dir / library));
        stores.emplace(std::string{library}, store);
        return store;
    }

    struct BlobAddress {
        blob::BlobStore store;
        Blake3Digest hash;
    };

    Result<BlobAddress> blob_address(const HttpServer::Request& request) {
        RR_TRY_ASSIGN(blob::BlobStore store, store_for(request.path_param("library").value_or("")));
        auto hash = Blake3Digest::from_hex(request.path_param("hash").value_or(""));
        if (!hash.has_value()) {
            return fail(Errc::invalid_argument, "the blob hash must be 64 hex characters (BLAKE3)");
        }
        return BlobAddress{.store = std::move(store), .hash = *hash};
    }

    Response blob_check(const HttpServer::Request& request) {
        if (request.method != net::Method::head) {
            Response response =
                Response::error(Error{Errc::invalid_argument, "blobs support HEAD and PUT only"});
            response.status = 405;
            return response;
        }
        watchdog.activity();
        auto address = blob_address(request);
        if (!address.has_value()) {
            return Response::error(address.error());
        }
        const auto stat = address->store.stat(address->hash);
        if (!stat.has_value()) {
            return Response::empty(404);
        }
        (void)address->store.touch(address->hash);  // about to be used: keep it from garbage collection
        Response response = Response::empty(200);
        response.headers.add("X-Blob-Size", std::to_string(stat->size));
        return response;
    }

    Response blob_upload(const HttpServer::Request& request, HttpServer::BodyReader& body) {
        const Watchdog::InFlight in_flight = watchdog.begin_request();
        auto address = blob_address(request);
        if (!address.has_value()) {
            return Response::error(address.error());
        }
        const bool existed = address->store.contains(address->hash);
        auto writer = address->store.begin_write(settings.max_upload_bytes);
        if (!writer.has_value()) {
            return Response::error(writer.error());
        }
        if (auto read = body.read([&](std::string_view chunk) { return writer->write(chunk); });
            !read.has_value()) {
            return Response::error(read.error());
        }
        const auto stored = writer->commit(address->hash);  // verified: the name must be the content's hash
        if (!stored.has_value()) {
            return Response::error(stored.error());
        }
        return Response::json(existed ? 200 : 201,
                              json::dump(json::Json{{"hash", stored->hash.hex()}, {"size", stored->size}}));
    }

    // --- Watchdog ---------------------------------------------------------------------
    void run_stop_command() {
        const auto now = clock.steady_now();
        if (stop_command_attempted.has_value() && now - *stop_command_attempted < kStopCommandRetry) {
            return;
        }
        stop_command_attempted = now;
        stop_decided = true;
        log::warn("idle for {} s with nothing running: stopping the pod", whole_seconds(watchdog.idle_for()));
        if (!settings.stop_command.empty()) {
            const auto result =
                proc::run(proc::Command{.argv = settings.stop_command}, proc::RunOptions{.timeout = 2min});
            if (!result.has_value()) {
                log::error("the stop command could not run: {}", result.error().describe());
            } else if (!result->status.success()) {
                log::error("the stop command failed ({}): {}", result->status.describe(),
                           result->stderr_tail(500));
            }
            return;
        }
        if (settings.stop_pod) {
            if (const auto stopped = settings.stop_pod(); !stopped.has_value()) {
                log::error("the pod could not stop itself: {}", stopped.error().describe());
            }
            return;
        }
        log::warn("no way to stop the pod is configured; it keeps running (and billing)");
    }

    void collect_garbage() {
        const auto cutoff =
            std::filesystem::file_time_type::clock::now() -
            std::chrono::duration_cast<std::filesystem::file_time_type::duration>(settings.blob_retention);
        std::vector<blob::BlobStore> snapshot;
        {
            const std::lock_guard lock(stores_mutex);
            for (const auto& [library, store] : stores) {
                snapshot.push_back(store);
            }
        }
        for (const blob::BlobStore& store : snapshot) {
            if (const auto removed = store.remove_unused_since(cutoff); !removed.has_value()) {
                log::warn("blob cleanup in '{}': {}", store.root().string(), removed.error().describe());
            } else if (*removed > 0) {
                log::info("removed {} unused blob(s) from '{}'", *removed, store.root().string());
            }
        }
    }

    void watch(const std::stop_token& stop) {
        auto last_collection = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            if (watchdog.should_stop()) {
                run_stop_command();
            }
            if (std::chrono::steady_clock::now() - last_collection >= kGarbageCollectionInterval) {
                collect_garbage();
                last_collection = std::chrono::steady_clock::now();
            }
            std::unique_lock lock(watch_mutex);
            (void)watch_wake.wait_for(lock, stop, settings.watchdog_interval, [] { return false; });
        }
    }

    // --- Wiring -------------------------------------------------------------------------
    void register_routes() {
        HttpServer& http = *server;
        http.set_guard([this](const HttpServer::Request& request) { return guard(request); });
        http.route(net::Method::get, std::string{runtime_api::kPathPing},
                   [](const auto&) { return Response::text(200, "ok"); });
        http.route(net::Method::get, std::string{runtime_api::kPathHealth},
                   [this](const auto&) { return Response::json(200, json::dump(to_json(health()))); });
        http.route(net::Method::post, std::string{runtime_api::kPathHeartbeat}, [this](const auto&) {
            watchdog.activity();
            return Response::empty(204);
        });
        http.route(net::Method::get, std::string{runtime_api::kPathModels},
                   [this](const auto&) { return list_models(); });
        for (const std::string_view path :
             {runtime_api::kPathChatCompletions, runtime_api::kPathEmbeddings, runtime_api::kPathRerank}) {
            http.route(
                net::Method::post, std::string{path},
                [this, path](const HttpServer::Request& request) { return proxy_to_engine(request, path); });
        }
        const std::string blobs{runtime_api::kPathBlobPattern};
        http.route(net::Method::get, blobs,
                   [this](const HttpServer::Request& request) { return blob_check(request); });
        http.route_streaming(net::Method::put, blobs,
                             [this](const HttpServer::Request& request, HttpServer::BodyReader& body) {
                                 return blob_upload(request, body);
                             });
        http.set_access_log([](const HttpServer::AccessLogEntry& entry) {
            if (entry.path != runtime_api::kPathPing) {
                log::debug("{} {} -> {} ({} ms)", entry.method, entry.path, entry.status,
                           entry.duration.count());
            }
        });
    }
};

Supervisor::Supervisor(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Supervisor::~Supervisor() {
    stop();
}

Result<std::unique_ptr<Supervisor>> Supervisor::create(SupervisorSettings settings) {
    if (settings.token.empty()) {
        return fail(Errc::invalid_argument, "the runtime needs a bearer token (server.token)");
    }
    const Profile* profile = settings.catalog.find_profile(settings.profile);
    if (profile == nullptr) {
        return fail(Errc::not_found, std::format("the catalog has no profile '{}'", settings.profile));
    }
    if (settings.blobs_dir.empty()) {
        return fail(Errc::invalid_argument, "the runtime needs a blobs directory");
    }
    const Clock& clock = settings.clock != nullptr ? *settings.clock : SystemClock::instance();
    auto impl = std::make_unique<Impl>(std::move(settings), clock);

    int port = impl->settings.first_engine_port;
    for (const EngineSpec& spec : profile->engines) {
        const Model& model = *impl->settings.catalog.find_model(spec.model);  // the catalog validated it
        const std::string model_id = model.id;
        auto on_output = [model_id, sink = impl->settings.on_engine_output](std::string_view line) {
            if (sink) {
                sink(model_id, line);
            }
        };
        auto engine = std::make_unique<Engine>(
            model.id, model.role, spec.slots,
            Engine::Settings{.command = llama_server_command(impl->settings.llama_server,
                                                             impl->settings.models_dir, model, spec, port),
                             .port = port,
                             .startup_timeout = impl->settings.engine_startup_timeout,
                             .stop_grace = impl->settings.engine_stop_grace,
                             .poll_interval = impl->settings.engine_poll_interval,
                             .restart_backoff = impl->settings.engine_restart_backoff,
                             .on_output = std::move(on_output)});
        impl->engine_for_model.emplace(model.id, engine.get());
        impl->engines.push_back(std::move(engine));
        ++port;
    }
    return std::unique_ptr<Supervisor>{new Supervisor(std::move(impl))};
}

Result<void> Supervisor::start() {
    Impl& impl = *impl_;
    std::error_code ec;
    std::filesystem::create_directories(impl.settings.blobs_dir, ec);
    if (ec) {
        return fail(Errc::io,
                    std::format("cannot create '{}': {}", impl.settings.blobs_dir.string(), ec.message()));
    }
    for (const auto& engine : impl.engines) {
        engine->start();
    }
    impl.server = std::make_unique<HttpServer>(HttpServer::Options{
        .host = impl.settings.host,
        .port = impl.settings.port,
        .worker_threads = impl.settings.worker_threads,
        .max_stream_bytes =
            static_cast<std::size_t>(impl.settings.max_upload_bytes) + (std::size_t{1} << 20U)});
    impl.register_routes();
    if (auto started = impl.server->start(); !started.has_value()) {
        stop();
        return started;
    }
    impl.watcher = std::jthread{[&impl](const std::stop_token& stop) { impl.watch(stop); }};
    return ok();
}

void Supervisor::stop() {
    Impl& impl = *impl_;
    // Relays waiting on an engine end first (with an honest error), so the
    // server below is not held up by them.
    impl.shutdown.request_stop();
    if (impl.watcher.joinable()) {
        impl.watcher.request_stop();
        impl.watch_wake.notify_all();
        impl.watcher.join();
    }
    if (impl.server) {
        impl.server->stop();  // no new requests; running ones finish
    }
    // Engines stop in parallel: each may take its full grace period.
    std::vector<std::jthread> stoppers;
    stoppers.reserve(impl.engines.size());
    for (const auto& engine : impl.engines) {
        stoppers.emplace_back([&engine] { engine->stop(); });
    }
}

int Supervisor::port() const noexcept {
    return impl_->server ? impl_->server->port() : 0;
}

runtime_api::HealthReport Supervisor::health() const {
    return impl_->health();
}

bool Supervisor::stop_requested() const noexcept {
    return impl_->stop_decided.load();
}

}  // namespace rr::supervisor
