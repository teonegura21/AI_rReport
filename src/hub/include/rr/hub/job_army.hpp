// rr/hub/job_army.hpp — a job's army, raised the first time a stage needs it.
//
// Why this exists
//   The pipeline's stages ask models through an army (rr/agents/army.hpp),
//   and are given the means to raise one rather than an army
//   (rr/pipeline/stage.hpp): a pod costs money from the minute it starts, so
//   a job whose stages need no model never starts it. JobArmy is that means,
//   for one job. Raising it:
//     1. The pod's engines. In live and record mode they come from the
//        runtime's health report, once every engine is ready; a pod RunPod
//        manages is started first if it is stopped, as `rreport pod start`
//        would. In replay mode no runtime is called: they come from the
//        report kept with the recordings (engines.json), which record mode
//        writes as it raises its army, so a replay raises the same one.
//     2. The stack every call goes through (rr/hub/model_stack.hpp: the
//        runtime and nowhere else, retries, accounting into llm_calls,
//        recording), and over it the job's memo (ADR-0020): a resumed job
//        asks again none of the calls it already made.
//     3. The army over them, with the job's ceilings (job_ceilings()), and
//        its elapsed time counted against the wall-clock one, logging its
//        missions in the library. The ceilings are per hour of the video,
//        so its length is asked when the army is raised: by then it is
//        known, since the source stage, which comes first, needs no model.
//   A failure is returned, and the next raise() tries again: a pod that did
//   not come up may come up later in the same run.
//
// Thread-safety: raise() may be called from several threads; one raises.
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>

#include <rr/agents/army.hpp>
#include <rr/core/clock.hpp>
#include <rr/core/result.hpp>
#include <rr/db/database.hpp>
#include <rr/hub/hub_config.hpp>
#include <rr/net/http_client.hpp>

namespace rr::hub {

// Where record mode keeps the engines it raised its army with, beside the
// recordings: what replay mode raises its army from.
[[nodiscard]] std::filesystem::path recorded_engines(const HubSettings& settings);

// A job's ceilings for a video of `length` (ADR-0018): `limits` per hour of
// it, a video shorter than limits.least_video counted as that long, the
// tokens rounded up. Some costs do not shrink with the video (the outline
// thinks over all of it, three times at `max`), and a ceiling below them
// would stop every short video.
[[nodiscard]] agents::Ceilings job_ceilings(std::chrono::milliseconds length, const JobLimits& limits);

class JobArmy {
public:
    struct Inputs {
        const HubSettings* settings = nullptr;
        const net::HttpClient* http = nullptr;
        db::Database* database = nullptr;  // the library's: the memo, the call log, the battle log
        const Clock* clock = nullptr;
        std::string job_id{};
        // Time the job spent before this run, counted against its wall-clock ceiling.
        std::chrono::milliseconds elapsed_before{};
        // How long the job's video lasts, asked when the army is raised; its
        // errors fail the raise. Unset: the job has no ceiling.
        std::function<Result<std::chrono::milliseconds>()> video_length{};
        std::stop_token stop{};
        std::function<void(std::string_view)> progress{};
    };

    // Everything `inputs` points to must outlive the army.
    explicit JobArmy(Inputs inputs);
    JobArmy(const JobArmy&) = delete;
    JobArmy& operator=(const JobArmy&) = delete;
    JobArmy(JobArmy&&) = delete;
    JobArmy& operator=(JobArmy&&) = delete;
    ~JobArmy();

    // The army, raised by the first call (see the header comment). Errors:
    //   Errc::needs_user        no runtime is configured, or no token for it,
    //                           or a replay finds no engines recorded;
    //   Errc::unavailable, …    the pod or its engines did not come up
    //                           (rr/runtime_client/pod_manager.hpp);
    //   Errc::invalid_argument  the engines cannot make an army
    //                           (rr/hub/army_engines.hpp);
    //   Errc::cancelled         on a stop.
    [[nodiscard]] Result<agents::Army*> raise();

private:
    struct Forces;

    Inputs inputs_;
    std::mutex mutex_;                // guards forces_
    std::unique_ptr<Forces> forces_;  // once raised
};

}  // namespace rr::hub
