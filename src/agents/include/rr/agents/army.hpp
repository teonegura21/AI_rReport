// rr/agents/army.hpp — missions run on the pod's engines: as many at once as
// each engine holds, escalated when their answers fail, within the job's
// ceilings, and logged.
//
// Why this exists
//   A video becomes hundreds of missions: dozens of windows to atomize, each
//   sampled three times, sections to draft four times, paragraphs to judge
//   (ADR-0011, ADR-0018). Doing that well on two engines needs one place
//   that knows what each engine holds and keeps every one of them busy:
//
//   * **Each engine has its own workers and queue (a battalion).** Its
//     queue is ordered by priority, then first come, first served. A worker
//     sends a mission's calls only within the engine's slots
//     (rr::Limiter, so llama-server never queues a request out of our
//     control) and, in a shared pool, within its tokens (rr/agents/sizing.hpp).
//   * **A mission moves between battalions; workers never wait for one
//     another.** A mission that escalates from the infantry to the
//     commander is queued with the commander's battalion, and the infantry
//     worker goes on to the next mission. A slow commander can never idle
//     the infantry (ADR-0011's "second wave").
//   * **Samples share a prompt.** A mission's k samples go to the engine as
//     one request for k answers wherever they fit (llama-server reads the
//     prompt once; plan P3), in several requests where they do not. Sample
//     j draws seed + j, so a split draws the same seeds as one request.
//   * **Answers are checked, repaired, escalated** (rr/agents/ladder.hpp):
//     each failing answer is sent back with its problems (Mission::repairs
//     times); a call that ran out while thinking is asked again without
//     thinking; a refusal goes to another family. A rung is climbed only
//     when no sample was accepted, and asks for one answer. The same
//     question is never asked twice: it would get the same answer.
//   * **Ceilings stop runaway jobs, not quality** (ADR-0018): once the job's
//     completion tokens or wall-clock time reach their ceiling, no call is
//     started; missions end with Verdict::ceiling and what they have, and
//     ceiling() says which ceiling, and when. Calls in flight finish.
//   * **The battle log** keeps one record per mission (BattleLog; the
//     library keeps them in its `missions` table). A record that cannot be
//     written is logged; it never fails the mission.
//
//   The army sits on the job's provider stack (its memo, retries and
//   accounting: rr/hub/model_stack.hpp), so a resumed job's missions are
//   answered from memory, a flaky pod is retried below it, and every call
//   is accounted.
//
// Failures
//   A mission ends with an Outcome whenever the models answered, however
//   badly: invalid and refused are findings a stage publishes. An Error is
//   for what is not the models' doing: a mission the army cannot run
//   (Errc::invalid_argument, Errc::unsupported), the job being stopped
//   (Errc::cancelled), a pod that stays unreachable after the provider's
//   retries. The first error stops the squad's other missions, and squad()
//   returns it: what they had answered is in the job's memo, and a resume
//   asks it again for free.
//
// Thread-safety: execute() and squad() may be called from many threads at
//   once, never from a mission's check (asserted): a check runs on a
//   battalion's worker, which would wait for itself.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <rr/agents/mission.hpp>
#include <rr/core/clock.hpp>
#include <rr/core/error.hpp>
#include <rr/core/result.hpp>
#include <rr/llm/chat.hpp>
#include <rr/llm/provider.hpp>
#include <rr/prompt/tokens.hpp>

namespace rr::agents {

// The job's safety ceilings (ADR-0018); none: no ceiling.
struct Ceilings {
    std::optional<std::int64_t> completion_tokens{};
    std::optional<std::chrono::milliseconds> wall_clock{};
};

enum class CeilingKind : std::uint8_t {
    completion_tokens,
    wall_clock
};

[[nodiscard]] std::string_view to_string(CeilingKind kind) noexcept;

// A ceiling reached: which one, where the job stood, and the mission that
// found it.
struct CeilingHit {
    CeilingKind kind = CeilingKind::completion_tokens;
    std::int64_t completion_tokens = 0;
    std::chrono::milliseconds elapsed{};
    std::string mission{};
};

// A mission, as the battle log keeps it.
struct MissionRecord {
    std::string job_id{};
    std::string mission{};
    std::string kind{};
    std::string prompt{};
    std::string role{};
    std::chrono::system_clock::time_point started_at{};
    std::chrono::milliseconds duration{};
    std::optional<Verdict> verdict{};  // none: it failed, with `error`
    std::optional<Error> error{};
    std::optional<Step> step{};  // the last rung climbed; none: none was
    int rungs = 0;
    std::string answered_by{};  // the model whose answers were accepted; empty: none
    int samples = 1;
    int accepted = 0;
    int calls = 0;
    llm::Usage usage{};
    std::vector<std::string> problems{};  // what was wrong with the last answer that failed
};

// Where missions are recorded. The library database keeps them
// (rr/library/mission_log.hpp); tests keep them in memory.
class BattleLog {
public:
    BattleLog() = default;
    BattleLog(const BattleLog&) = delete;
    BattleLog& operator=(const BattleLog&) = delete;
    BattleLog(BattleLog&&) = delete;
    BattleLog& operator=(BattleLog&&) = delete;
    virtual ~BattleLog() = default;

    // Called from the army's workers, concurrently. A mission run again (a
    // resumed job) replaces its record.
    [[nodiscard]] virtual Result<void> record(const MissionRecord& mission) = 0;
};

class Army {
public:
    struct Options {
        // The engines, from the runtime's health report. Every role is
        // served by one engine; an engine may serve several roles (on a pod
        // with no infantry, the commander's engine serves both).
        std::vector<Engine> engines{};
        std::string job_id{};  // tags every call; required with a battle log
        Ceilings ceilings{};
        // Time the job spent before this army, counted against the
        // wall-clock ceiling (a resumed job).
        std::chrono::milliseconds elapsed_before{};
        // Sizes prompts (rr/agents/sizing.hpp): calibrate it from the call
        // log; the default errs high.
        prompt::TokenEstimator estimator{};
    };

    struct Spent {
        std::int64_t calls = 0;
        llm::Usage usage{};  // over every call, answered from a job's memo too
        std::chrono::milliseconds elapsed{};
    };

    // An army for `options`, or Errc::invalid_argument saying what is wrong
    // with them. `provider` and `clock`, and `log` when given, must outlive
    // the army.
    [[nodiscard]] static Result<Army> create(Options options, const llm::LlmProvider& provider,
                                             const Clock& clock, BattleLog* log = nullptr);

    Army(Army&&) noexcept;
    Army& operator=(Army&&) noexcept;
    Army(const Army&) = delete;
    Army& operator=(const Army&) = delete;
    // Waits for the workers; no mission may be running.
    ~Army();

    // Runs one mission to its outcome (see the header comment).
    [[nodiscard]] Result<Outcome> execute(Mission mission, const std::stop_token& stop = {});

    // Runs missions side by side; their outcomes come back in their order.
    // Every mission is checked before any runs. Errc::invalid_argument for a
    // mission the army cannot run, or two with one id.
    [[nodiscard]] Result<std::vector<Outcome>> squad(std::vector<Mission> missions,
                                                     const std::stop_token& stop = {});

    // What the job has spent through this army, and the elapsed time before it.
    [[nodiscard]] Spent spent() const;

    // The first ceiling reached, if one was.
    [[nodiscard]] std::optional<CeilingHit> ceiling() const;

    // Missions queued for an engine's worker, not yet on their way: what a
    // progress display shows as waiting.
    [[nodiscard]] std::size_t waiting() const;

    [[nodiscard]] std::span<const Engine> engines() const noexcept;

private:
    struct Impl;
    explicit Army(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace rr::agents
