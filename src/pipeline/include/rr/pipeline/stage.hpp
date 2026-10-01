// rr/pipeline/stage.hpp — one step from a video to its report, and the key its
// result is kept under.
//
// Why this exists
//   A job is a chain of stages (plan §5): source, transcript, then outline,
//   atomize, plan, write. Each turns its inputs, the artifacts of the stages
//   before it, into one artifact (ADR-0004). A Stage says what it reads and
//   makes; the executor (rr/pipeline/executor.hpp) runs it, or finds it done.
//
// The cache key
//   A stage's key is the BLAKE3 hash of the canonical JSON of:
//     * its name and version. The version goes up when what the stage makes
//       from the same inputs changes: a new rule, a fixed bug;
//     * its fingerprint: what else its result depends on, as the stage says.
//       That means its prompts' versions, the models it asks and how they
//       sample (army_fingerprint()), its settings;
//     * the hashes of its inputs' artifacts, by stage;
//     * for a stage that samples, the job's salt (ADR-0020). A new salt asks
//       new samples, and the same salt finds the old ones.
//   Equal keys mean equal results. So a key found in the stage cache is a
//   stage done, by this job or by another, and a prompt that changes runs
//   again only the stages that use it. The stages after them run again only
//   where the new artifact differs from the old: their keys hold its hash,
//   not the key that made it.
//
// The army, raised when needed
//   Models run on the pod, and a pod costs money from the minute it starts.
//   So a stage is given the means to raise the army, not the army: the
//   first stage that needs a model raises it (which may start the pod and
//   wait for its engines), and a job whose stages need none, such as a video
//   with its creator's captions today, never starts the pod. A stage that
//   asks models raises the army in its fingerprint too: its key names the
//   models that will answer.
//
// Thread-safety: a Stage is immutable; run() may be called from any thread.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <rr/agents/army.hpp>
#include <rr/core/result.hpp>
#include <rr/json/json.hpp>
#include <rr/library/jobs.hpp>
#include <rr/prompt/library.hpp>

namespace rr::pipeline {

// Raises the job's army, or returns the one raised before. Errors are the
// pod's: Errc::needs_user when it is not set up, Errc::unavailable when its
// engines do not come up, Errc::cancelled on a stop.
using RaiseArmy = std::function<Result<agents::Army*>()>;

// What a stage may use while it runs.
struct StageContext {
    const library::Job* job = nullptr;
    // The artifacts of the stages it reads, by stage name.
    std::map<std::string, json::Json, std::less<>> inputs{};
    // What its key was made from, set once fingerprint() has said: run()
    // makes its result from the same, so that the key says what made it.
    json::Json fingerprint{};
    // Empty when no pod is configured: then no model can be asked, and a
    // stage that needs one does what it can without, and says so.
    RaiseArmy army{};
    const prompt::Library* prompts = nullptr;
    std::filesystem::path work_directory{};  // the job's own scratch space
    std::stop_token stop{};
    // How the stage is doing, a line at a time, for the user to read.
    std::function<void(std::string_view)> progress{};
};

// What a stage made.
struct StageOutput {
    json::Json artifact{};
    // What it could not do: published with the result, never hidden.
    std::vector<std::string> findings{};
    // false when the result depends on this job and not only on the key:
    // a job ceiling cut it short (ADR-0018). It is then kept for this job
    // alone (rr/library/stage_store.hpp).
    bool reusable = true;
};

class Stage {
public:
    Stage() = default;
    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;
    Stage(Stage&&) = delete;
    Stage& operator=(Stage&&) = delete;
    virtual ~Stage() = default;

    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual int version() const = 0;
    // The stages whose artifacts it reads; they run before it.
    [[nodiscard]] virtual std::vector<std::string> inputs() const = 0;
    // The schema its artifact follows ("artifacts/transcript"): checked
    // before the artifact is kept.
    [[nodiscard]] virtual std::string_view schema() const = 0;
    // Whether it samples: its key then holds the job's salt.
    [[nodiscard]] virtual bool samples() const { return false; }
    // What else its result depends on (the header comment). Called before
    // run(), with the same context.
    [[nodiscard]] virtual Result<json::Json> fingerprint(const StageContext& context) const = 0;
    [[nodiscard]] virtual Result<StageOutput> run(const StageContext& context) const = 0;
};

// `stage`'s key (the header comment); `inputs` are its inputs' artifact
// hashes, by stage.
[[nodiscard]] std::string cache_key(const Stage& stage, const json::Json& fingerprint,
                                    const std::map<std::string, std::string, std::less<>>& inputs,
                                    std::int64_t salt);

// What a stage's results owe to the army's models: each engine's model, the
// checksum of the file it serves, its roles and how it samples, in the order
// of their models. The engines' sizes are left out: how many slots answer
// changes how fast, not what.
[[nodiscard]] json::Json army_fingerprint(const agents::Army& army);

}  // namespace rr::pipeline
