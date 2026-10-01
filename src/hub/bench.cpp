#include <rr/hub/bench.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>
#include <thread>
#include <utility>

#include <rr/log/log.hpp>
#include <rr/net/retry.hpp>

namespace rr::hub {

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// Plain ASCII, so cutting it anywhere leaves valid text.
constexpr std::string_view kRecord =
    "The committee met on a grey morning to weigh the reports from every district. The first report "
    "described the harvest, which had been late but plentiful, and the second the state of the roads, "
    "several of which had flooded in the spring. A long discussion followed about the bridge at the ford, "
    "whose repair had been promised twice and postponed twice. The treasurer read the accounts aloud, "
    "noting each expense and the reason given for it, and the clerk wrote down every objection. ";

double seconds(milliseconds duration) {
    return std::chrono::duration<double>(duration).count();
}

template <class T>
T median(std::vector<T> values) {
    if (values.empty()) {
        return T{};
    }
    std::ranges::sort(values);
    return values[values.size() / 2];
}

template <class T>
T percentile_95(std::vector<T> values) {
    if (values.empty()) {
        return T{};
    }
    std::ranges::sort(values);
    const auto rank = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(values.size())));
    return values[std::max<std::size_t>(rank, 1) - 1];
}

llm::ChatRequest bench_request(const std::string& model, std::string prompt, int output_tokens,
                               int choices = 1) {
    llm::ChatRequest request{.model = model,
                             .messages = {llm::Message::user(std::move(prompt))},
                             .max_tokens = output_tokens,
                             .temperature = 0.0,
                             .seed = 1,
                             .thinking = false,
                             // Exactly max_tokens tokens, each prompt read in full.
                             .extra = {{"ignore_eos", true}, {"cache_prompt", false}},
                             .tag = {.purpose = "bench"}};
    if (choices > 1) {
        // Several answers need sampling: at temperature 0 they would all be the
        // same, and such a request is refused. Each still runs to max_tokens.
        request.n = choices;
        request.temperature = 1.0;
    }
    return request;
}

milliseconds first_token_of(const llm::ChatResponse& answer) {
    return answer.first_token.value_or(answer.duration);
}

// The engine's own speed when it reports it; else what the client measured.
double generation_speed(const llm::ChatResponse& answer) {
    if (answer.generation_tokens_per_second.has_value()) {
        return *answer.generation_tokens_per_second;
    }
    const double generating = seconds(answer.duration - first_token_of(answer));
    return generating > 0 ? static_cast<double>(answer.usage.completion_tokens) / generating : 0.0;
}

double prompt_speed(const llm::ChatResponse& answer) {
    if (answer.prompt_tokens_per_second.has_value()) {
        return *answer.prompt_tokens_per_second;
    }
    const double reading = seconds(first_token_of(answer));
    return reading > 0 ? static_cast<double>(answer.usage.prompt_tokens) / reading : 0.0;
}

// Samples the VRAM in use until stopped; remembers the peak (all GPUs together).
class VramWatch {
public:
    VramWatch(const runtime_client::RuntimeClient& runtime, milliseconds every)
        : thread_([this, &runtime, every](const std::stop_token& stop) {
            while (!stop.stop_requested()) {
                if (auto health = runtime.health(stop); health.has_value()) {
                    std::int64_t used = 0;
                    for (const runtime_api::GpuInfo& gpu : health->gpus) {
                        used += gpu.memory_used_mib;
                    }
                    const std::lock_guard lock(mutex_);
                    peak_ = std::max(peak_, used);
                }
                (void)net::pause_for(every, stop);
            }
        }) {}

    [[nodiscard]] std::int64_t peak() const {
        const std::lock_guard lock(mutex_);
        return peak_;
    }

private:
    mutable std::mutex mutex_;
    std::int64_t peak_ = 0;
    std::jthread thread_;  // last: stopped and joined before the members above go
};

Result<CommanderResult> bench_commander(const llm::LlmProvider& provider, const std::string& model,
                                        const BenchOptions& options, const std::stop_token& stop,
                                        const BenchProgress& progress) {
    std::vector<milliseconds> first_tokens;
    std::vector<double> prompt_speeds;
    std::vector<double> generation_speeds;
    for (int round = 1; round <= options.rounds; ++round) {
        if (progress) {
            progress(std::format("commander '{}': request {} of {}", model, round, options.rounds));
        }
        RR_TRY_ASSIGN(const llm::ChatResponse answer,
                      with_context(provider.chat(bench_request(model, bench_prompt(options.prompt_tokens),
                                                               options.output_tokens, options.choices),
                                                 {}, stop),
                                   std::format("benchmarking the commander '{}'", model)));
        first_tokens.push_back(first_token_of(answer));
        prompt_speeds.push_back(prompt_speed(answer));
        generation_speeds.push_back(generation_speed(answer));
    }
    return CommanderResult{.model = model,
                           .rounds = options.rounds,
                           .choices = options.choices,
                           .first_token_p50 = median(first_tokens),
                           .prompt_tokens_per_second = median(prompt_speeds),
                           .generation_tokens_per_second = median(generation_speeds)};
}

Result<InfantryRow> bench_parallel(const llm::LlmProvider& provider, const std::string& model, int parallel,
                                   int slots, const BenchOptions& options, const std::stop_token& stop) {
    std::vector<Result<llm::ChatResponse>> answers(static_cast<std::size_t>(parallel));
    const auto started = Clock::now();
    {
        std::vector<std::jthread> requests;
        requests.reserve(answers.size());
        for (std::size_t index = 0; index < answers.size(); ++index) {
            requests.emplace_back([&, index] {
                // A different start for each request: no two share a cached prefix.
                std::string prompt = std::format("Request {} of {}.\n\n{}", index + 1, parallel,
                                                 bench_prompt(options.prompt_tokens));
                answers[index] =
                    provider.chat(bench_request(model, std::move(prompt), options.output_tokens), {}, stop);
            });
        }
    }  // every request has ended here
    InfantryRow row{.parallel = parallel,
                    .slots = slots,
                    .wall = std::chrono::duration_cast<milliseconds>(Clock::now() - started)};
    std::vector<double> speeds;
    std::vector<milliseconds> first_tokens;
    const Error* first_failure = nullptr;
    for (const Result<llm::ChatResponse>& answer : answers) {
        if (!answer.has_value()) {
            ++row.failed;
            first_failure = first_failure != nullptr ? first_failure : &answer.error();
            continue;
        }
        row.tokens += answer->usage.completion_tokens;
        speeds.push_back(generation_speed(*answer));
        first_tokens.push_back(first_token_of(*answer));
    }
    if (row.failed == parallel) {
        return fail(Error{*first_failure}.with_context(
            std::format("every one of {} parallel requests to '{}' failed", parallel, model)));
    }
    if (first_failure != nullptr) {
        log::warn("{} of {} parallel requests to '{}' failed: {}", row.failed, parallel, model,
                  first_failure->describe());
    }
    row.total_tokens_per_second =
        seconds(row.wall) > 0 ? static_cast<double>(row.tokens) / seconds(row.wall) : 0;
    row.per_request_tokens_per_second = median(speeds);
    row.first_token_p50 = median(first_tokens);
    row.first_token_p95 = percentile_95(first_tokens);
    return row;
}

const runtime_api::EngineInfo* engine_for(const runtime_api::HealthReport& health, std::string_view model,
                                          std::string_view role) {
    for (const runtime_api::EngineInfo& engine : health.engines) {
        if (model.empty() ? engine.role == role : engine.model == model) {
            return &engine;
        }
    }
    return nullptr;
}

std::string dollars_per_million(const std::optional<double>& price_per_hour, double tokens_per_second) {
    if (!price_per_hour.has_value() || tokens_per_second <= 0) {
        return "–";
    }
    return std::format("${:.2f}", *price_per_hour / (tokens_per_second * 3600.0) * 1e6);
}

}  // namespace

std::string bench_prompt(int tokens) {
    std::string prompt =
        "Here is a long record. Read it, then continue it in the same style for as long as you can.\n\n";
    const std::size_t size = static_cast<std::size_t>(std::max(tokens, 32)) * 4;
    while (prompt.size() < size) {
        prompt += kRecord;
    }
    prompt.resize(size);
    return prompt;
}

Result<BenchResult> run_bench(const llm::LlmProvider& provider, const runtime_client::RuntimeClient& runtime,
                              const BenchOptions& options, const std::stop_token& stop,
                              const BenchProgress& progress) {
    BenchResult result{.started_at = std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(
                                                                  std::chrono::system_clock::now())),
                       .price_per_hour = options.price_per_hour};
    RR_TRY_ASSIGN(const runtime_api::HealthReport health, runtime.health(stop));
    result.runtime_version = health.version;
    result.profile = health.profile;
    result.gpus = health.gpus;

    const runtime_api::EngineInfo* commander = engine_for(health, options.commander, "commander");
    const runtime_api::EngineInfo* infantry = engine_for(health, options.infantry, "infantry");
    if (commander == nullptr && infantry == nullptr) {
        return fail(Errc::not_found,
                    std::format("the runtime (profile {}) has no commander or infantry engine to measure",
                                health.profile));
    }

    // llama-server starts a request for n answers only when n slots are free at
    // once: with fewer slots in all, it would hold the request forever.
    if (commander != nullptr && options.choices > commander->slots) {
        return fail(Errc::invalid_argument,
                    std::format("{} answers per request need {} commander slots at once, and '{}' has {}",
                                options.choices, options.choices, commander->model, commander->slots));
    }
    const VramWatch vram{runtime, options.vram_poll};
    if (commander != nullptr) {
        RR_TRY_ASSIGN(result.commander, bench_commander(provider, commander->model, options, stop, progress));
    }
    if (infantry != nullptr) {
        result.infantry_model = infantry->model;
        for (const int parallel : options.parallel) {
            if (progress) {
                progress(std::format("infantry '{}': {} request(s) at once", infantry->model, parallel));
            }
            RR_TRY_ASSIGN(const InfantryRow row, bench_parallel(provider, infantry->model, parallel,
                                                                infantry->slots, options, stop));
            result.infantry.push_back(row);
        }
    }
    result.peak_vram_used_mib = vram.peak();
    return result;
}

std::string to_markdown(const BenchResult& result) {
    std::string text = std::format("# rReport bench, {}\n\n", result.started_at);
    std::int64_t total_mib = 0;
    for (const runtime_api::GpuInfo& gpu : result.gpus) {
        text += std::format("- GPU {}: {}, {} MiB\n", gpu.index, gpu.name, gpu.memory_total_mib);
        total_mib += gpu.memory_total_mib;
    }
    text += std::format("- Profile {}, runtime {}\n", result.profile, result.runtime_version);
    text += std::format("- Peak VRAM in use: {} MiB of {} MiB\n", result.peak_vram_used_mib, total_mib);
    if (result.cold_start.has_value()) {
        text += std::format("- Cold start: {:.0f} s from starting the pod to every engine ready\n",
                            seconds(*result.cold_start));
    }
    text += result.price_per_hour.has_value()
                ? std::format("- Price: ${:.2f} per hour\n", *result.price_per_hour)
                : std::string{"- Price: unknown (pass --price-per-hour)\n"};
    if (result.commander.has_value()) {
        const CommanderResult& commander = *result.commander;
        text += std::format(
            "\n## Commander `{}`{}\n\n| first token (p50) | prompt tok/s | generated tok/s | per million "
            "generated |\n"
            "|---:|---:|---:|---:|\n| {} ms | {:.0f} | {:.1f} | {} |\n",
            commander.model,
            commander.choices > 1 ? std::format(", {} answers per request", commander.choices)
                                  : std::string{},
            commander.first_token_p50.count(), commander.prompt_tokens_per_second,
            commander.generation_tokens_per_second,
            dollars_per_million(result.price_per_hour, commander.generation_tokens_per_second));
    }
    if (!result.infantry.empty()) {
        text += std::format(
            "\n## Infantry `{}`\n\n| at once | slots | total tok/s | per request tok/s | first token "
            "p50 / p95 | failed | per million generated |\n|---:|---:|---:|---:|---:|---:|---:|\n",
            result.infantry_model);
        for (const InfantryRow& row : result.infantry) {
            text += std::format("| {}{} | {} | {:.1f} | {:.1f} | {} / {} ms | {} | {} |\n", row.parallel,
                                row.parallel > row.slots ? " (queued)" : "", row.slots,
                                row.total_tokens_per_second, row.per_request_tokens_per_second,
                                row.first_token_p50.count(), row.first_token_p95.count(), row.failed,
                                dollars_per_million(result.price_per_hour, row.total_tokens_per_second));
        }
    }
    return text;
}

json::Json to_json(const BenchResult& result) {
    json::Json gpus = json::Json::array();
    for (const runtime_api::GpuInfo& gpu : result.gpus) {
        gpus.push_back(
            {{"index", gpu.index}, {"name", gpu.name}, {"memory_total_mib", gpu.memory_total_mib}});
    }
    json::Json document{
        {"started_at", result.started_at},
        {"runtime_version", result.runtime_version},
        {"profile", result.profile},
        {"gpus", std::move(gpus)},
        {"peak_vram_used_mib", result.peak_vram_used_mib},
        {"cold_start_ms",
         result.cold_start.has_value() ? json::Json(result.cold_start->count()) : json::Json()},
        {"price_per_hour",
         result.price_per_hour.has_value() ? json::Json(*result.price_per_hour) : json::Json()}};
    if (result.commander.has_value()) {
        document["commander"] = {
            {"model", result.commander->model},
            {"rounds", result.commander->rounds},
            {"choices", result.commander->choices},
            {"first_token_p50_ms", result.commander->first_token_p50.count()},
            {"prompt_tokens_per_second", result.commander->prompt_tokens_per_second},
            {"generation_tokens_per_second", result.commander->generation_tokens_per_second}};
    }
    json::Json rows = json::Json::array();
    for (const InfantryRow& row : result.infantry) {
        rows.push_back({{"parallel", row.parallel},
                        {"slots", row.slots},
                        {"failed", row.failed},
                        {"tokens", row.tokens},
                        {"wall_ms", row.wall.count()},
                        {"total_tokens_per_second", row.total_tokens_per_second},
                        {"per_request_tokens_per_second", row.per_request_tokens_per_second},
                        {"first_token_p50_ms", row.first_token_p50.count()},
                        {"first_token_p95_ms", row.first_token_p95.count()}});
    }
    document["infantry"] = {{"model", result.infantry_model}, {"rows", std::move(rows)}};
    return document;
}

}  // namespace rr::hub
