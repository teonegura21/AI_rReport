#include <rr/agents/army.hpp>

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <utility>

#include <rr/agents/ladder.hpp>
#include <rr/agents/sizing.hpp>
#include <rr/core/assert.hpp>
#include <rr/core/limiter.hpp>
#include <rr/core/strings.hpp>
#include <rr/core/thread_pool.hpp>
#include <rr/llm/sampling.hpp>
#include <rr/llm/structured.hpp>
#include <rr/log/log.hpp>

namespace rr::agents {

namespace {

constexpr std::string_view kProseRepairInstruction =
    "Your answer cannot be used. Problems:\n{problems}\n"
    "Reply with the corrected answer only, and nothing else.";

// The army whose battalion this thread works for. A mission's check runs
// here, and must never call that army: the worker would wait for itself.
const void*& serving() noexcept {
    thread_local const void* army = nullptr;
    return army;
}

// The seed a mission's request carries: check() refuses a mission without.
std::int64_t seed_of(const llm::ChatRequest& request) {
    RR_ASSERT(request.seed.has_value(), "every mission carries a seed");
    return *request.seed;
}

void add(llm::Usage& total, const llm::Usage& usage) noexcept {
    total.prompt_tokens += usage.prompt_tokens;
    total.completion_tokens += usage.completion_tokens;
    total.cached_prompt_tokens += usage.cached_prompt_tokens;
}

// Every sampling field unset, for an engine's preset to fill whole.
void clear_sampling(llm::ChatRequest& request) noexcept {
    request.temperature.reset();
    request.top_p.reset();
    request.top_k.reset();
    request.min_p.reset();
    request.presence_penalty.reset();
    request.repeat_penalty.reset();
    request.thinking.reset();
    request.thinking_budget.reset();
    request.reasoning_effort.reset();
}

// A prose answer: its text, which the stage's check reads as a JSON string.
llm::CheckedAnswer check_prose(const llm::Choice& answer, const llm::AnswerCheck& check) {
    llm::CheckedAnswer checked;
    if (trim(answer.content).empty()) {
        checked.problems.emplace_back("the answer is empty");
    } else if (check) {
        checked.problems = check(json::Json(answer.content));
    }
    // Prose has no closing brace to show it is whole: a cut is always a problem.
    if (answer.finish == llm::FinishReason::length) {
        checked.problems.emplace_back("the answer was cut off at the token limit, so it must be shorter");
    }
    if (checked.usable()) {
        checked.value = json::Json(answer.content);
    }
    return checked;
}

llm::CheckedAnswer read(const Mission& mission, const llm::Choice& answer) {
    return mission.schema ? llm::check_structured(answer, *mission.schema, mission.check)
                          : check_prose(answer, mission.check);
}

bool refuses(const Mission& mission, const llm::Choice& answer) {
    if (answer.finish == llm::FinishReason::content_filter) {
        return true;
    }
    return mission.refusal ? mission.refusal(answer) : looks_like_refusal(answer.content);
}

std::string_view repair_instruction(const Mission& mission) noexcept {
    if (!mission.repair_instruction.empty()) {
        return mission.repair_instruction;
    }
    return mission.schema ? llm::kRepairInstruction : kProseRepairInstruction;
}

// One engine's workers, queue and limits.
class Battalion {
public:
    using Leg = std::move_only_function<void()>;

    Battalion(const Engine& engine, const void* army)
        : engine_(&engine)
        , army_(army)
        , slots_(static_cast<std::size_t>(engine.slots))
        , pool_(engine.kv_unified ? std::make_unique<Limiter>(static_cast<std::size_t>(engine.context))
                                  : nullptr)
        , workers_(static_cast<std::size_t>(engine.slots), std::format("army:{}", engine.model),
                   [model = engine.model](const std::exception_ptr&) {
                       log::error("a worker for '{}' failed outside any mission", model);
                   }) {}

    Battalion(const Battalion&) = delete;
    Battalion& operator=(const Battalion&) = delete;
    Battalion(Battalion&&) = delete;
    Battalion& operator=(Battalion&&) = delete;

    ~Battalion() {
        const std::lock_guard lock(mutex_);
        RR_ASSERT(queue_.empty(), "an army is destroyed only once its missions have ended");
    }

    // Queues `leg`. It runs on one of the battalion's workers once no leg of
    // a higher priority, or of the same priority and queued earlier, waits.
    void post(Priority priority, Leg leg) {
        {
            const std::lock_guard lock(mutex_);
            queue_.push_back(
                Queued{.priority = priority.value, .sequence = next_sequence_++, .leg = std::move(leg)});
            std::ranges::push_heap(queue_, runs_later);
        }
        // One worker task per leg. It runs the best leg waiting when it
        // starts, which need not be the one it was queued with.
        workers_.post([this] { run_next(); });
    }

    [[nodiscard]] std::size_t waiting() const {
        const std::lock_guard lock(mutex_);
        return queue_.size();
    }

    [[nodiscard]] const Engine& engine() const noexcept { return *engine_; }
    [[nodiscard]] Limiter& slots() noexcept { return slots_; }
    // The shared pool's tokens; null when each slot has its own share.
    [[nodiscard]] Limiter* pool() noexcept { return pool_.get(); }

private:
    struct Queued {
        int priority;
        std::uint64_t sequence;
        Leg leg;
    };

    // The heap's order: its front is the highest priority, queued first.
    static bool runs_later(const Queued& lhs, const Queued& rhs) noexcept {
        if (lhs.priority != rhs.priority) {
            return lhs.priority < rhs.priority;
        }
        return lhs.sequence > rhs.sequence;
    }

    void run_next() {
        Leg leg;
        {
            const std::lock_guard lock(mutex_);
            RR_ASSERT(!queue_.empty(), "every worker task has a leg queued with it");
            std::ranges::pop_heap(queue_, runs_later);
            leg = std::move(queue_.back().leg);
            queue_.pop_back();
        }
        const void* const previous = std::exchange(serving(), army_);
        leg();
        serving() = previous;
    }

    const Engine* engine_;
    const void* army_;
    Limiter slots_;
    std::unique_ptr<Limiter> pool_;
    mutable std::mutex mutex_;  // guards the queue
    std::vector<Queued> queue_;
    std::uint64_t next_sequence_ = 0;
    ThreadPool workers_;  // last: its threads use everything above
};

// The missions of one squad() call, and how they ended.
struct SquadState {
    explicit SquadState(std::size_t missions) : outcomes(missions), remaining(missions) {}

    std::mutex mutex;  // guards everything but `stop`
    std::condition_variable done;
    std::vector<std::optional<Outcome>> outcomes;
    std::size_t remaining;
    std::optional<Error> error;  // the first
    std::stop_source stop;       // requested by the caller, or at the first error
};

// One mission while it runs. Its legs run one after another, each on the
// battalion of its rung, so one thread at a time works on it.
struct MissionRun {
    MissionRun(Mission work, std::size_t position, std::shared_ptr<SquadState> owner,
               std::vector<log::Field> fields, Ladder climb, const Clock& clock)
        : mission(std::move(work))
        , index(position)
        , squad(std::move(owner))
        , context(std::move(fields))
        , ladder(std::move(climb))
        , rung(ladder.first())
        , started(clock.steady_now())
        , started_at(clock.now()) {
        outcome.mission = mission.id;
    }

    Mission mission;
    std::size_t index;
    std::shared_ptr<SquadState> squad;
    std::vector<log::Field> context;  // the caller's log fields
    Ladder ladder;
    Rung rung;                    // the rung being climbed
    std::set<std::string> asked;  // the keys of the requests made
    Outcome outcome;
    std::chrono::steady_clock::time_point started;
    std::chrono::system_clock::time_point started_at;
};

// What one rung gave.
struct Battle {
    std::vector<Answer> answers;
    std::vector<std::string> problems;  // of the last answer that failed
    int refusals = 0;
    bool ceiling = false;  // a ceiling stopped it
};

// What is wrong with one engine (nothing: ok()).
Result<void> check_engine(const Engine& engine) {
    const auto about = [&](std::string_view why) {
        return fail(Errc::invalid_argument, std::format("engine '{}': {}", engine.model, why));
    };
    if (engine.model.empty()) {
        return fail(Errc::invalid_argument, "an engine needs a model");
    }
    if (engine.roles.empty() || std::ranges::any_of(engine.roles, &std::string::empty)) {
        return about("it serves no role, or one without a name");
    }
    if (engine.family.empty()) {
        return about("it needs a family (family_of())");
    }
    if (engine.slots < 1 || engine.ctx_per_slot < 1 || engine.context < engine.ctx_per_slot) {
        return about("it needs a slot, and a context that holds its sequences");
    }
    if (const auto problems = llm::preset_problems(engine.direct); !problems.empty()) {
        return about(std::format("its direct preset: {}", join(problems, "; ")));
    }
    if (engine.direct.thinking) {
        return about("its direct preset thinks");
    }
    if (const auto problems = llm::preset_problems(engine.thinking); !problems.empty()) {
        return about(std::format("its thinking preset: {}", join(problems, "; ")));
    }
    if (!engine.thinking.thinking || !engine.thinking.thinking_budget.has_value()) {
        return about("its thinking preset must think, with a budget, so that every escalation ends");
    }
    return ok();
}

Result<void> check_options(const Army::Options& options, const BattleLog* log) {
    if (options.engines.empty()) {
        return fail(Errc::invalid_argument, "an army needs an engine");
    }
    std::set<std::string_view> models;
    std::set<std::string_view> roles;
    for (const Engine& engine : options.engines) {
        RR_TRY(check_engine(engine));
        if (!models.insert(engine.model).second) {
            return fail(Errc::invalid_argument, std::format("engine '{}' is listed twice", engine.model));
        }
        for (const std::string& role : engine.roles) {
            if (!roles.insert(role).second) {
                return fail(Errc::invalid_argument,
                            std::format("the role '{}' is served by two engines", role));
            }
        }
    }
    if (log != nullptr && options.job_id.empty()) {
        return fail(Errc::invalid_argument, "a battle log needs a job id");
    }
    if (options.ceilings.completion_tokens.value_or(1) < 1 ||
        options.ceilings.wall_clock.value_or(std::chrono::milliseconds{1}).count() < 1) {
        return fail(Errc::invalid_argument, "a ceiling must be above zero");
    }
    return ok();
}

}  // namespace

std::string_view to_string(CeilingKind kind) noexcept {
    switch (kind) {
        case CeilingKind::completion_tokens:
            return "completion_tokens";
        case CeilingKind::wall_clock:
            return "wall_clock";
    }
    return "completion_tokens";
}

struct Army::Impl {
    Impl(Options settings, const llm::LlmProvider& models, const Clock& time, BattleLog* battle)
        : options(std::move(settings))
        , provider(&models)
        , clock(&time)
        , battle_log(battle)
        , caps(models.caps())
        , start(time.steady_now()) {
        for (std::size_t engine = 0; engine < options.engines.size(); ++engine) {
            for (const std::string& role : options.engines[engine].roles) {
                by_role.emplace(role, engine);
            }
        }
        if (const auto found = by_role.find("commander"); found != by_role.end()) {
            commander = found->second;
        }
        battalions.reserve(options.engines.size());
        for (const Engine& engine : options.engines) {
            battalions.push_back(std::make_unique<Battalion>(engine, this));
        }
    }

    [[nodiscard]] Result<void> check(const Mission& mission) const;
    [[nodiscard]] llm::ChatRequest request_for(const Mission& mission, const Rung& rung) const;
    [[nodiscard]] bool holds(const Mission& mission, const Rung& rung) const;

    void launch(Mission mission, std::size_t index, const std::shared_ptr<SquadState>& squad,
                const std::vector<log::Field>& context);
    void run_leg(std::unique_ptr<MissionRun> run);
    [[nodiscard]] Result<bool> fight(MissionRun& run);
    [[nodiscard]] Result<void> fight_rung(MissionRun& run, Battle& battle);
    [[nodiscard]] Result<void> settle(MissionRun& run, const llm::ChatRequest& asked, int sample,
                                      llm::Choice answer, Battle& battle);
    [[nodiscard]] Result<llm::ChatResponse> call(MissionRun& run, const llm::ChatRequest& request,
                                                 const Footprint& footprint, int choices);
    [[nodiscard]] bool over_ceiling(std::string_view mission);
    void finish(std::unique_ptr<MissionRun> run, std::optional<Error> error) const;
    void record(const MissionRun& run, const std::optional<Error>& error) const;
    [[nodiscard]] std::chrono::milliseconds elapsed() const;

    const Options options;
    const llm::LlmProvider* provider;
    const Clock* clock;
    BattleLog* battle_log;
    const llm::ProviderCaps caps;
    std::map<std::string, std::size_t, std::less<>> by_role;
    std::optional<std::size_t> commander;
    const std::chrono::steady_clock::time_point start;

    mutable std::mutex mutex;  // guards spent and hit
    Spent spent{};             // calls and usage; elapsed is computed
    std::optional<CeilingHit> hit;

    std::vector<std::unique_ptr<Battalion>> battalions;  // last: their workers use everything above
};

Result<void> Army::Impl::check(const Mission& mission) const {
    if (mission.id.empty()) {
        return fail(Errc::invalid_argument, "a mission needs an id");
    }
    const auto refuse = [&](std::string_view why) {
        return fail(Errc::invalid_argument, std::format("mission '{}': {}", mission.id, why));
    };
    if (mission.kind.empty()) {
        return refuse("it needs a kind");
    }
    const auto own = by_role.find(mission.role);
    if (own == by_role.end()) {
        return refuse(std::format("no engine serves the role '{}'", mission.role));
    }
    const llm::ChatRequest& request = mission.request;
    if (request.messages.empty()) {
        return refuse("it has no messages");
    }
    if (!request.model.empty() || request.max_tokens.has_value() || request.n.has_value() ||
        request.json_schema.has_value() || request.tag != llm::CallTag{}) {
        return refuse(
            "the army sets a request's model, max_tokens, n, json_schema and tag; a mission names a role, "
            "answer_tokens, samples and a schema instead");
    }
    if (mission.answer_tokens < 1 || mission.samples < 1 || mission.repairs < 0) {
        return refuse("it needs answer_tokens and samples of at least 1, and repairs of at least 0");
    }
    if (!request.seed.has_value()) {
        return refuse("it has no seed: derive one (llm::derived_seed())");
    }
    if (const auto penalties = llm::penalty_problems(request); !penalties.empty()) {
        return refuse(std::format("penalties bias an answer's labels and paraphrase quotes: {}",
                                  join(penalties, "; ")));
    }
    if (request.thinking.value_or(false) && !caps.thinking) {
        return refuse(std::format("it asks to think, and '{}' cannot switch thinking", provider->name()));
    }
    const Rung first{.step = Step::as_ordered,
                     .engine = own->second,
                     .thinking = request.thinking.value_or(false) && caps.thinking};
    const llm::ChatRequest asked = request_for(mission, first);
    if (const auto problems = llm::sampling_problems(asked); !problems.empty()) {
        return refuse(join(problems, "; "));
    }
    if (mission.samples > 1 && asked.temperature.value_or(1.0) == 0.0) {
        return refuse("its samples would all be one answer: greedy decoding draws nothing");
    }
    auto footprint = footprint_of(asked, options.estimator);
    if (!footprint.has_value()) {
        return std::unexpected(
            std::move(footprint).error().with_context(std::format("mission '{}'", mission.id)));
    }
    const Engine& engine = options.engines[own->second];
    if (choices_that_fit(engine, *footprint) < 1) {
        return refuse(
            std::format("it needs about {} tokens (a prompt of {}, an answer of up to {}), and '{}' holds "
                        "{} in a sequence: split its packet",
                        footprint->prompt + footprint->answer, footprint->prompt, footprint->answer,
                        engine.model, engine.ctx_per_slot));
    }
    return ok();
}

llm::ChatRequest Army::Impl::request_for(const Mission& mission, const Rung& rung) const {
    const Engine& engine = options.engines[rung.engine];
    llm::ChatRequest request = mission.request;
    if (rung.step != Step::as_ordered) {
        clear_sampling(request);  // an escalation samples as its engine does
    }
    request.thinking = rung.thinking;
    if (!rung.thinking) {
        request.thinking_budget.reset();
        request.reasoning_effort.reset();
    }
    llm::apply(rung.thinking ? engine.thinking : engine.direct, request);
    std::int64_t max_tokens = mission.answer_tokens;
    if (rung.thinking && request.thinking_budget.has_value()) {
        // The thinking, its closing words, then the whole answer.
        max_tokens +=
            request.thinking_budget->tokens + options.estimator.estimate(request.thinking_budget->closing);
    }
    request.max_tokens =
        static_cast<int>(std::min<std::int64_t>(max_tokens, std::numeric_limits<int>::max()));
    if (!caps.thinking) {
        request.thinking.reset();  // what the provider cannot switch, it is not asked to
    }
    request.model = engine.model;
    request.tag = llm::CallTag{.purpose = mission.kind, .job_id = options.job_id, .mission_id = mission.id};
    return mission.schema ? llm::structured_request(std::move(request), *mission.schema, caps) : request;
}

bool Army::Impl::holds(const Mission& mission, const Rung& rung) const {
    const auto footprint = footprint_of(request_for(mission, rung), options.estimator);
    return footprint.has_value() && choices_that_fit(options.engines[rung.engine], *footprint) >= 1;
}

void Army::Impl::launch(Mission mission, std::size_t index, const std::shared_ptr<SquadState>& squad,
                        const std::vector<log::Field>& context) {
    const std::size_t own = by_role.find(mission.role)->second;
    Ladder ladder{options.engines,   own, commander, mission.request.thinking.value_or(false), caps.thinking,
                  mission.escalation};
    const Priority priority = mission.priority;
    auto run =
        std::make_unique<MissionRun>(std::move(mission), index, squad, context, std::move(ladder), *clock);
    battalions[own]->post(priority, [this, run = std::move(run)]() mutable { run_leg(std::move(run)); });
}

void Army::Impl::run_leg(std::unique_ptr<MissionRun> run) {
    const log::ScopedContext context{run->context};
    const log::ScopedField field{"mission", run->mission.id};
    Result<bool> climb = [&]() -> Result<bool> {
        // A stage's check is code like any other: if it throws, the mission
        // fails with the bug, instead of taking the worker down.
        try {
            return fight(*run);
        } catch (const std::exception& error) {
            return fail(Errc::internal, std::format("mission '{}' threw: {}", run->mission.id, error.what()));
        } catch (...) {
            return fail(Errc::internal, std::format("mission '{}' threw", run->mission.id));
        }
    }();
    if (!climb.has_value()) {
        finish(std::move(run), std::move(climb).error());
        return;
    }
    if (*climb) {
        // The next rung's engine has its own workers: this one goes on with
        // the next mission meanwhile.
        Battalion& next = *battalions[run->rung.engine];
        const Priority priority = run->mission.priority;
        next.post(priority, [this, run = std::move(run)]() mutable { run_leg(std::move(run)); });
        return;
    }
    finish(std::move(run), std::nullopt);
}

Result<bool> Army::Impl::fight(MissionRun& run) {
    Outcome& outcome = run.outcome;
    if (over_ceiling(run.mission.id)) {
        outcome.verdict = Verdict::ceiling;
        return false;
    }
    ++outcome.rungs;
    outcome.step = run.rung.step;
    Battle battle;
    RR_TRY(fight_rung(run, battle));
    outcome.answers = std::move(battle.answers);
    if (!battle.problems.empty()) {
        outcome.problems = std::move(battle.problems);  // a rung that failed nothing keeps the last rung's
    }
    if (battle.ceiling) {
        outcome.verdict = Verdict::ceiling;
        return false;
    }
    if (!outcome.answers.empty()) {
        outcome.verdict = Verdict::accepted;
        return false;
    }
    const bool refused = battle.refusals > 0;
    const std::optional<Rung> next =
        run.ladder.next(refused, [&](const Rung& rung) { return holds(run.mission, rung); });
    if (!next.has_value()) {
        outcome.verdict = refused ? Verdict::refused : Verdict::invalid;
        return false;
    }
    run.rung = *next;
    return true;
}

Result<void> Army::Impl::fight_rung(MissionRun& run, Battle& battle) {
    const Engine& engine = options.engines[run.rung.engine];
    // Past the first rung, the mission is being rescued: one answer will do.
    const int samples = run.rung.step == Step::as_ordered ? run.mission.samples : 1;
    const llm::ChatRequest asked = request_for(run.mission, run.rung);
    RR_TRY_ASSIGN(const Footprint footprint, footprint_of(asked, options.estimator));
    for (int first = 0; first < samples;) {
        if (first > 0 && over_ceiling(run.mission.id)) {
            battle.ceiling = true;
            return ok();
        }
        const int choices = std::min(samples - first, choices_that_fit(engine, footprint));
        RR_ASSERT(choices >= 1, "a rung is climbed only when its engine holds the mission");
        llm::ChatRequest request = asked;
        request.seed = seed_of(asked) + first;  // choice i draws seed + i: sample first + i's seed
        if (choices > 1) {
            request.n = choices;
        }
        RR_TRY_ASSIGN(llm::ChatResponse response, call(run, request, footprint, choices));
        for (int choice = 0; choice < choices; ++choice) {
            RR_TRY(settle(run, asked, first + choice,
                          std::move(response.choices[static_cast<std::size_t>(choice)]), battle));
        }
        first += choices;
    }
    return ok();
}

Result<void> Army::Impl::settle(MissionRun& run, const llm::ChatRequest& asked, int sample,
                                llm::Choice answer, Battle& battle) {
    const Engine& engine = options.engines[run.rung.engine];
    // The conversation that gave `answer`, as a request for it alone.
    llm::ChatRequest conversation = asked;
    conversation.n.reset();
    conversation.seed = seed_of(asked) + sample;
    for (int repairs = 0;; ++repairs) {
        llm::CheckedAnswer checked = read(run.mission, answer);
        if (checked.usable()) {
            battle.answers.push_back(Answer{.value = std::move(checked.value),
                                            .choice = std::move(answer),
                                            .sample = sample,
                                            .step = run.rung.step,
                                            .model = engine.model,
                                            .thinking = conversation.thinking.value_or(false),
                                            .repairs = repairs});
            return ok();
        }
        battle.problems = std::move(checked.problems);
        if (refuses(run.mission, answer)) {
            battle.problems.emplace_back("it reads as a refusal");
            ++battle.refusals;
            return ok();  // repairing a refusal would only ask the same model again
        }
        if (repairs >= run.mission.repairs) {
            return ok();
        }
        llm::ChatRequest next;
        if (conversation.thinking.value_or(false) && answer.finish == llm::FinishReason::length) {
            // ADR-0018: a call that ran out while thinking is asked again,
            // without thinking. It has no answer to repair.
            Rung plain = run.rung;
            plain.thinking = false;
            next = request_for(run.mission, plain);
            next.seed = seed_of(next) + sample;
        } else {
            next =
                llm::repair_request(conversation, answer, battle.problems, repair_instruction(run.mission));
        }
        if (run.asked.contains(llm::request_key(provider->name(), next))) {
            return ok();  // asked already: it would get the same answer
        }
        RR_TRY_ASSIGN(const Footprint footprint, footprint_of(next, options.estimator));
        if (choices_that_fit(engine, footprint) < 1) {
            battle.problems.push_back(std::format("its repair would not fit '{}'", engine.model));
            return ok();
        }
        if (over_ceiling(run.mission.id)) {
            battle.ceiling = true;
            return ok();
        }
        RR_TRY_ASSIGN(llm::ChatResponse response, call(run, next, footprint, 1));
        conversation = std::move(next);
        answer = std::move(response.first());
    }
}

Result<llm::ChatResponse> Army::Impl::call(MissionRun& run, const llm::ChatRequest& request,
                                           const Footprint& footprint, int choices) {
    const auto context = [&] {
        return std::format("while running mission '{}' on '{}'", run.mission.id, request.model);
    };
    Battalion& battalion = *battalions[run.rung.engine];
    const std::stop_token stop = run.squad->stop.get_token();
    if (stop.stop_requested()) {
        return fail(Errc::cancelled, context());  // even with a slot free
    }
    const Priority priority = run.mission.priority;
    auto slots = battalion.slots().acquire(stop, priority, Slots{static_cast<std::size_t>(choices)});
    if (!slots.has_value()) {
        return std::unexpected(std::move(slots).error().with_context(context()));
    }
    std::optional<Limiter::Permit> room;
    if (Limiter* pool = battalion.pool()) {
        const std::int64_t tokens = reservation(battalion.engine(), footprint, choices);
        auto reserved = pool->acquire(stop, priority, Slots{static_cast<std::size_t>(tokens)});
        if (!reserved.has_value()) {
            return std::unexpected(std::move(reserved).error().with_context(context()));
        }
        room = std::move(*reserved);
    }
    run.asked.insert(llm::request_key(provider->name(), request));
    ++run.outcome.calls;
    {
        const std::lock_guard lock(mutex);
        ++spent.calls;
    }
    Result<llm::ChatResponse> response = provider->chat(request, llm::StreamSink{}, stop);
    room.reset();
    slots->release();
    if (!response.has_value()) {
        return std::unexpected(std::move(response).error().with_context(context()));
    }
    if (std::cmp_less(response->choices.size(), choices)) {
        return fail(Errc::internal, std::format("'{}' gave {} answer(s) to a request for {}", request.model,
                                                response->choices.size(), choices));
    }
    add(run.outcome.usage, response->usage);
    {
        const std::lock_guard lock(mutex);
        add(spent.usage, response->usage);
    }
    return response;
}

bool Army::Impl::over_ceiling(std::string_view mission) {
    const std::chrono::milliseconds now = elapsed();
    const std::lock_guard lock(mutex);
    if (hit.has_value()) {
        return true;  // once reached, a ceiling stays reached
    }
    std::optional<CeilingKind> kind;
    if (options.ceilings.completion_tokens.has_value() &&
        spent.usage.completion_tokens >= *options.ceilings.completion_tokens) {
        kind = CeilingKind::completion_tokens;
    } else if (options.ceilings.wall_clock.has_value() && now >= *options.ceilings.wall_clock) {
        kind = CeilingKind::wall_clock;
    }
    if (!kind.has_value()) {
        return false;
    }
    hit = CeilingHit{.kind = *kind,
                     .completion_tokens = spent.usage.completion_tokens,
                     .elapsed = now,
                     .mission = std::string{mission}};
    log::warn("the job reached its {} ceiling ({} completion tokens, {} ms): no further model call starts",
              to_string(*kind), hit->completion_tokens, now.count());
    return true;
}

void Army::Impl::finish(std::unique_ptr<MissionRun> run, std::optional<Error> error) const {
    run->outcome.duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(clock->steady_now() - run->started);
    record(*run, error);
    const bool failed = error.has_value();
    const std::shared_ptr<SquadState> squad = run->squad;  // outlives `run`
    {
        const std::lock_guard lock(squad->mutex);
        if (failed) {
            if (!squad->error.has_value()) {
                squad->error = std::move(*error);
            }
        } else {
            squad->outcomes[run->index] = std::move(run->outcome);
        }
        --squad->remaining;
    }
    if (failed) {
        squad->stop.request_stop();  // the others stop too: the memo keeps what they had
    }
    squad->done.notify_all();
}

void Army::Impl::record(const MissionRun& run, const std::optional<Error>& error) const {
    if (battle_log == nullptr) {
        return;
    }
    const Outcome& outcome = run.outcome;
    const MissionRecord record{
        .job_id = options.job_id,
        .mission = run.mission.id,
        .kind = run.mission.kind,
        .prompt = run.mission.prompt,
        .role = run.mission.role,
        .started_at = run.started_at,
        .duration = outcome.duration,
        .verdict = error.has_value() ? std::nullopt : std::optional{outcome.verdict},
        .error = error,
        .step = outcome.rungs > 0 ? std::optional{outcome.step} : std::nullopt,
        .rungs = outcome.rungs,
        .answered_by = outcome.answers.empty() ? std::string{} : outcome.answers.front().model,
        .samples = run.mission.samples,
        .accepted = static_cast<int>(outcome.answers.size()),
        .calls = outcome.calls,
        .usage = outcome.usage,
        .problems = outcome.problems};
    if (const auto written = battle_log->record(record); !written.has_value()) {
        log::error("mission '{}' was not written to the battle log: {}", run.mission.id,
                   written.error().describe());
    }
}

std::chrono::milliseconds Army::Impl::elapsed() const {
    return options.elapsed_before +
           std::chrono::duration_cast<std::chrono::milliseconds>(clock->steady_now() - start);
}

Result<Army> Army::create(Options options, const llm::LlmProvider& provider, const Clock& clock,
                          BattleLog* log) {
    RR_TRY(check_options(options, log));
    return Army{std::make_unique<Impl>(std::move(options), provider, clock, log)};
}

Army::Army(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Army::Army(Army&&) noexcept = default;
Army& Army::operator=(Army&&) noexcept = default;
Army::~Army() = default;

Result<Outcome> Army::execute(Mission mission, const std::stop_token& stop) {
    std::vector<Mission> one;
    one.push_back(std::move(mission));
    RR_TRY_ASSIGN(std::vector<Outcome> outcomes, squad(std::move(one), stop));
    return std::move(outcomes.front());
}

Result<std::vector<Outcome>> Army::squad(std::vector<Mission> missions, const std::stop_token& stop) {
    RR_ASSERT(serving() != impl_.get(), "a mission's check must not call the army that runs it");
    std::set<std::string_view> ids;
    for (const Mission& mission : missions) {
        RR_TRY(impl_->check(mission));
        if (!ids.insert(mission.id).second) {
            return fail(Errc::invalid_argument, std::format("two missions are named '{}'", mission.id));
        }
    }
    if (missions.empty()) {
        return std::vector<Outcome>{};
    }
    const auto squad = std::make_shared<SquadState>(missions.size());
    SquadState* const state = squad.get();
    const std::stop_callback forward{stop, [state] { state->stop.request_stop(); }};
    const std::vector<log::Field> context = log::context_snapshot();
    for (std::size_t index = 0; index < missions.size(); ++index) {
        impl_->launch(std::move(missions[index]), index, squad, context);
    }

    std::unique_lock lock(squad->mutex);
    squad->done.wait(lock, [&] { return squad->remaining == 0; });
    if (squad->error.has_value()) {
        return std::unexpected(*squad->error);
    }
    std::vector<Outcome> outcomes;
    outcomes.reserve(squad->outcomes.size());
    for (std::optional<Outcome>& outcome : squad->outcomes) {
        RR_ASSERT(outcome.has_value(), "every mission of a squad that did not fail has its outcome");
        outcomes.push_back(std::move(*outcome));
    }
    return outcomes;
}

Army::Spent Army::spent() const {
    const std::chrono::milliseconds elapsed = impl_->elapsed();
    const std::lock_guard lock(impl_->mutex);
    Spent spent = impl_->spent;
    spent.elapsed = elapsed;
    return spent;
}

std::optional<CeilingHit> Army::ceiling() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->hit;
}

std::size_t Army::waiting() const {
    std::size_t waiting = 0;
    for (const std::unique_ptr<Battalion>& battalion : impl_->battalions) {
        waiting += battalion->waiting();
    }
    return waiting;
}

std::span<const Engine> Army::engines() const noexcept {
    return impl_->options.engines;
}

}  // namespace rr::agents
