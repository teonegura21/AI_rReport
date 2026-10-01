// rr/pipeline/executor.hpp — a job's stages, each found done or run, and kept.
//
// Why this exists
//   ADR-0004's promises are kept here:
//     * Resumable. Each stage's key is looked up first. A hit is a stage
//       done, by this job before it stopped or by another job, and costs
//       nothing.
//     * Crash-consistent. A stage that runs is committed with its artifact,
//       its cache entry and the job's record in one transaction
//       (rr/library/stage_store.hpp), and only once its artifact follows its
//       schema.
//     * Honest. A stage that fails is recorded with why, and stops the job:
//       the stages after it would have nothing to read.
//   A stage stopped halfway loses none of the answers it had: the job's memo
//   holds them, and a resume asks again for free (ADR-0020).
//
// Thread-safety: run() may be called for different jobs at once.
#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rr/core/clock.hpp>
#include <rr/core/error.hpp>
#include <rr/core/result.hpp>
#include <rr/library/jobs.hpp>
#include <rr/library/stage_store.hpp>
#include <rr/pipeline/stage.hpp>

namespace rr::pipeline {

// One stage of a run.
struct StageReport {
    std::string stage{};
    bool cached = false;                  // found done
    std::string artifact{};               // its hash
    std::vector<std::string> findings{};  // what it could not do
    std::chrono::milliseconds took{};
};

// What a run did: its stages in order, as far as they went.
struct RunReport {
    std::vector<StageReport> stages{};
    std::optional<Error> error{};  // what stopped it, when something did
};

class Executor {
public:
    // An executor of `stages`, given in an order where each stage comes after
    // the stages it reads. Errc::invalid_argument when they are not, or when
    // two share a name. `clock` must outlive the executor.
    [[nodiscard]] static Result<Executor> create(std::vector<std::unique_ptr<Stage>> stages,
                                                 const Clock& clock);

    // Takes `job` through the stages (see the header comment). `context`
    // gives the army, the prompts, the work directory and the stop; the
    // executor sets the job and each stage's inputs. The first failure ends
    // the run, recorded in `store` and in the report.
    [[nodiscard]] RunReport run(const library::Job& job, library::StageStore& store,
                                StageContext context) const;

    // The status a job takes after `error` stopped it. It is stopped when a
    // resume can go on: a stop was asked for, a ceiling was reached, or the
    // pod or the network failed for now. It needs the user when a person must
    // act first, and failed otherwise.
    [[nodiscard]] static library::JobStatus status_after(const Error& error) noexcept;

private:
    Executor(std::vector<std::unique_ptr<Stage>> stages, const Clock& clock);

    std::vector<std::shared_ptr<const Stage>> stages_;
    const Clock* clock_;
};

}  // namespace rr::pipeline
