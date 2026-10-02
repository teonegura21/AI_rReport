// The evaluation's commands (eval_commands.hpp). Each wires rr_eval's
// modules together; the rules live in src/eval/, where they are unit-tested.
#include "eval_commands.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>

#include <rr/core/file_io.hpp>
#include <rr/db/database.hpp>
#include <rr/eval/annotate.hpp>
#include <rr/eval/compare.hpp>
#include <rr/eval/export.hpp>
#include <rr/eval/flow.hpp>
#include <rr/eval/gate.hpp>
#include <rr/eval/gold.hpp>
#include <rr/eval/history.hpp>
#include <rr/eval/judgement.hpp>
#include <rr/eval/metrics.hpp>
#include <rr/eval/panel.hpp>
#include <rr/eval/run.hpp>
#include <rr/eval/score.hpp>
#include <rr/eval/set.hpp>
#include <rr/eval/sheet.hpp>
#include <rr/eval/snapshot.hpp>
#include <rr/eval/stats.hpp>
#include <rr/eval/sweep.hpp>
#include <rr/hub/job_army.hpp>
#include <rr/hub/model_stack.hpp>
#include <rr/library/jobs.hpp>
#include <rr/library/library.hpp>
#include <rr/library/stage_store.hpp>
#include <rr/net/http_client.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/pipeline/stage.hpp>
#include <rr/proc/termination_signals.hpp>
#include <rr/prompt/library.hpp>
#include <rr/reason/tier.hpp>

namespace rr::cli {

namespace {

using namespace rr::hub;

// ---------------------------------------------------------------------------------------
// eval fetch, annotate, sheet
// ---------------------------------------------------------------------------------------
struct EvalFlags {
    std::filesystem::path root{"eval"};
    std::string set{"m2"};
    std::vector<std::string> videos;           // none: every video of the set
    bool again = false;                        // fetch: read a video kept already once more
    bool replace = false;                      // annotate: draft a silver gold file again
    std::optional<std::filesystem::path> out;  // sheet, export, report: where to write it
    std::string label;                         // run, score, export: the run's
    std::string tier{reason::kDefaultTier};    // run: a tier, or a variant of one (max+write.thinking=true)
    int replicates = 3;                        // run: three at least, to see the noise (ADR-0019)
    std::string split{"dev"};                  // run: dev, or test for a gate
    std::string format{"csv"};                 // export: csv or json
    std::string panel{"eval-v1"};              // judge, gate, compare --flow: the frozen panel
                                               // (eval/judges/<panel>.toml)
    int first_replicate = 0;                   // run: an A/A run starts after its baseline's last
    std::string baseline;                      // compare, gate: run A, the baseline
    std::string candidate;                     // compare, gate: run B, the candidate
    std::string gate{"default"};               // compare, gate: eval/gates/<gate>.toml
    std::optional<std::string> metric;         // compare: one metric, video by video; report: the chart's
    bool flow = false;                         // compare: the panel reads the runs' articles pairwise
    bool aa = false;                           // gate: B is A run again: check the gate's margins
    std::string sweep;                         // sweep: eval/sweeps/<sweep>.toml
    bool no_judge = false;                     // sweep: score by code alone
};

// The command that does `verb` to `video`, with the flags that differ from
// the defaults: what the user is told to run next.
std::string eval_command(const EvalFlags& flags, std::string_view verb, std::string_view video) {
    std::string command = std::format("rreport eval {}", verb);
    if (flags.root != EvalFlags{}.root) {
        command += std::format(" --eval-dir {}", flags.root.string());
    }
    if (flags.set != EvalFlags{}.set) {
        command += std::format(" --eval-set {}", flags.set);
    }
    return std::format("{} {}", command, video);
}

// The set the flags name. One exists once its candidates are approved.
Result<eval::EvalSet> eval_set(const EvalFlags& flags) {
    auto set = eval::read_set(flags.root, flags.set);
    if (!set.has_value() && set.error().code() == Errc::not_found) {
        return fail(Errc::not_found,
                    std::format("{} does not exist: a set is made from the approved candidates ({})",
                                (flags.root / "sets" / std::format("{}.toml", flags.set)).string(),
                                (flags.root / "README.md").string()));
    }
    return set;
}

// The videos of `set` the command names, or all of them.
Result<std::vector<eval::EvalVideo>> chosen(const eval::EvalSet& set, const std::vector<std::string>& ids) {
    if (ids.empty()) {
        return set.videos;
    }
    std::vector<eval::EvalVideo> videos;
    videos.reserve(ids.size());
    for (const std::string& id : ids) {
        const eval::EvalVideo* video = set.find(id);
        if (video == nullptr) {
            return fail(Errc::not_found, std::format("the set '{}' has no video '{}'", set.name, id));
        }
        videos.push_back(*video);
    }
    return videos;
}

// Says what went wrong with one video, and goes on with the next.
int video_failed(const eval::EvalVideo& video, const Error& error) {
    (void)std::fprintf(stderr, "rreport: %s: %s\n", video.id.c_str(), error.describe().c_str());
    return kFailure;
}

int eval_fetch(const Global& global, const EvalFlags& flags, const proc::TerminationSignals& signals) {
    auto hub = load(global);
    if (!hub.has_value()) {
        return failed(kBadConfiguration, hub.error());
    }
    auto set = eval_set(flags);
    if (!set.has_value()) {
        return failed(kBadConfiguration, set.error());
    }
    auto videos = chosen(*set, flags.videos);
    if (!videos.has_value()) {
        return failed(kBadConfiguration, videos.error());
    }
    const HubSettings& settings = hub->settings;
    const pipeline::SourceStage stage{
        media::YtDlp{media::YtDlpOptions{.executable = settings.ytdlp, .timeout = settings.ytdlp_timeout}}};
    int code = kOk;
    for (const eval::EvalVideo& video : *videos) {
        if (!flags.again) {
            // Read once: a snapshot kept is never read again unless asked.
            auto kept = eval::read_snapshot(flags.root, video);
            if (!kept.has_value()) {
                code = video_failed(video, kept.error());
                continue;
            }
            if (kept->has_value()) {
                (void)std::printf("%s: kept already, snapshot %s\n", video.id.c_str(), (*kept)->hash.c_str());
                continue;
            }
        }
        const std::filesystem::path work =
            settings.library_database().parent_path() / "eval-fetch" / video.id;
        auto snapshot = eval::fetch_snapshot(flags.root, video, stage, work, signals.stop_token(),
                                             std::chrono::system_clock::now());
        std::error_code ignored;  // scratch: what is kept is in the eval's directory
        std::filesystem::remove_all(work, ignored);
        if (!snapshot.has_value()) {
            if (snapshot.error().code() == Errc::cancelled) {
                return interrupted_or(kFailure, snapshot.error(), signals);
            }
            code = video_failed(video, snapshot.error());
            continue;
        }
        (void)std::printf("%s: kept %s, snapshot %s\n", video.id.c_str(),
                          eval::snapshot_file(flags.root, video.id).string().c_str(), snapshot->hash.c_str());
        if (!video.snapshot.has_value()) {
            (void)std::printf("  pin it in the set, under the video: snapshot = \"%s\"\n",
                              snapshot->hash.c_str());
        }
    }
    return code;
}

int eval_annotate(const Global& global, const EvalFlags& flags, const proc::TerminationSignals& signals) {
    auto hub = load(global);
    if (!hub.has_value()) {
        return failed(kBadConfiguration, hub.error());
    }
    auto set = eval_set(flags);
    if (!set.has_value()) {
        return failed(kBadConfiguration, set.error());
    }
    auto videos = chosen(*set, flags.videos);
    if (!videos.has_value()) {
        return failed(kBadConfiguration, videos.error());
    }
    const HubSettings& settings = hub->settings;
    auto database = library::open_database(settings.library_database());
    if (!database.has_value()) {
        return failed(kFailure, database.error());
    }
    auto prompts = prompt::Library::embedded();
    if (!prompts.has_value()) {
        return failed(kFailure, prompts.error());
    }
    const net::HttpClient http = make_http();
    int code = kOk;
    for (const eval::EvalVideo& video : *videos) {
        auto job =
            eval::start_annotation(flags.root, video, *database, SystemClock::instance(), flags.replace);
        if (!job.has_value()) {
            code = video_failed(video, job.error());
            continue;
        }
        (void)std::fprintf(stderr, "job %s: annotating %s, %s\n", job->id.c_str(), video.id.c_str(),
                           video.title.c_str());
        JobArmy forces{JobArmy::Inputs{.settings = &settings,
                                       .http = &http,
                                       .database = &*database,
                                       .clock = &SystemClock::instance(),
                                       .job_id = job->id,
                                       .elapsed_before = job->elapsed,
                                       .video_length = video_length_of(*database, job->id),
                                       .stop = signals.stop_token(),
                                       .progress = say}};
        pipeline::StageContext context{.prompts = &*prompts,
                                       .work_directory = settings.library_database().parent_path() / "jobs",
                                       .stop = signals.stop_token(),
                                       .progress = say};
        if (settings.llm_mode == LlmMode::replay || settings.runtime.has_value()) {
            context.army = [&forces] { return forces.raise(); };
        }
        auto annotated = eval::run_annotation(flags.root, video, *std::move(job), *database,
                                              SystemClock::instance(), std::move(context));
        if (!annotated.has_value()) {
            code = video_failed(video, annotated.error());
            continue;
        }
        const int done = report_run(annotated->run, eval_command(flags, "annotate", video.id), signals);
        if (annotated->gold.has_value()) {
            const eval::Gold& gold = *annotated->gold;
            const auto core = std::ranges::count(gold.facts, eval::Importance::core, &eval::Fact::importance);
            (void)std::printf(
                "  %zu facts (%lld core), %zu must-quotes and %zu terms, drafted into %s\n"
                "  correct it with its sheet: %s\n",
                gold.facts.size(), static_cast<long long>(core), gold.must_quotes.size(), gold.terms.size(),
                eval::gold_file(flags.root, video.id).string().c_str(),
                eval_command(flags, "sheet", video.id).c_str());
        }
        if (done != kOk) {
            code = done;
            if (signals.received() != 0) {
                return done;  // interrupted: the next videos wait for the next run
            }
        }
    }
    return code;
}

int eval_sheet(const EvalFlags& flags) {
    auto set = eval_set(flags);
    if (!set.has_value()) {
        return failed(kBadConfiguration, set.error());
    }
    auto videos = chosen(*set, flags.videos);
    if (!videos.has_value()) {
        return failed(kBadConfiguration, videos.error());
    }
    const eval::EvalVideo& video = videos->front();
    auto snapshot = eval::read_snapshot(flags.root, video);
    if (!snapshot.has_value()) {
        return failed(kFailure, snapshot.error());
    }
    if (!snapshot->has_value()) {
        return failed(kFailure,
                      Error{Errc::not_found, std::format("{} has no snapshot yet: {} reads it", video.id,
                                                         eval_command(flags, "fetch", video.id))});
    }
    const std::filesystem::path file = eval::gold_file(flags.root, video.id);
    auto text = read_file(file);
    if (!text.has_value()) {
        return failed(kFailure, text.error().code() == Errc::not_found
                                    ? Error{Errc::not_found,
                                            std::format("{} does not exist yet: {} drafts it", file.string(),
                                                        eval_command(flags, "annotate", video.id))}
                                    : text.error());
    }
    auto gold = eval::parse_gold(*text, file.string());
    if (!gold.has_value()) {
        return failed(kFailure, gold.error());
    }
    auto transcript = eval::caption_transcript((*snapshot)->source);
    if (!transcript.has_value()) {
        return failed(kFailure, transcript.error());
    }
    const std::string sheet = eval::review_sheet(*gold, video, *transcript, (*snapshot)->hash);
    if (!flags.out.has_value()) {
        (void)std::fputs(sheet.c_str(), stdout);
        return kOk;
    }
    if (auto written = write_file_atomically(*flags.out, sheet); !written.has_value()) {
        return failed(kFailure, written.error());
    }
    (void)std::printf("wrote %s\n", flags.out->string().c_str());
    return kOk;
}

// ---------------------------------------------------------------------------------------
// eval run, score, export
// ---------------------------------------------------------------------------------------

// The run the flags ask for: `--split` read, `--tier` checked.
Result<eval::RunSpec> run_spec(const EvalFlags& flags, const eval::EvalSet& set) {
    RR_TRY(reason::find_tier(flags.tier));
    eval::Split split = eval::Split::dev;
    if (flags.split == eval::to_string(eval::Split::test)) {
        split = eval::Split::test;
    } else if (flags.split != eval::to_string(eval::Split::dev)) {
        return fail(Errc::invalid_argument, std::format("--split is dev or test, not '{}'", flags.split));
    }
    return eval::RunSpec{.label = flags.label,
                         .eval_set = set.name,
                         .split = split,
                         .tier = flags.tier,
                         .replicates = flags.replicates,
                         .first_replicate = flags.first_replicate};
}

// The command that takes the run up again, as it was asked.
std::string resume_run_command(const EvalFlags& flags) {
    std::string asked = std::format("--label {} --tier {} --replicates {} --split {}", flags.label,
                                    flags.tier, flags.replicates, flags.split);
    if (flags.first_replicate != EvalFlags{}.first_replicate) {
        asked += std::format(" --first-replicate {}", flags.first_replicate);
    }
    return eval_command(flags, "run", asked);
}

int eval_run(const Global& global, const EvalFlags& flags, const proc::TerminationSignals& signals) {
    auto hub = load(global);
    if (!hub.has_value()) {
        return failed(kBadConfiguration, hub.error());
    }
    auto set = eval_set(flags);
    if (!set.has_value()) {
        return failed(kBadConfiguration, set.error());
    }
    auto videos = chosen(*set, flags.videos);
    if (!videos.has_value()) {
        return failed(kBadConfiguration, videos.error());
    }
    auto spec = run_spec(flags, *set);
    if (!spec.has_value()) {
        return failed(kBadConfiguration, spec.error());
    }
    auto database = library::open_database(hub->settings.library_database());
    if (!database.has_value()) {
        return failed(kFailure, database.error());
    }
    eval::Runs runs{*database, SystemClock::instance()};
    auto run = runs.open(*spec);
    if (!run.has_value()) {
        // A label of another run, or a run of no video: what was asked is wrong.
        const bool asked_wrong =
            run.error().code() == Errc::conflict || run.error().code() == Errc::invalid_argument;
        return failed(asked_wrong ? kBadConfiguration : kFailure, run.error());
    }
    auto planned = eval::plan_run(flags.root, *run, *videos, runs);
    if (!planned.has_value()) {
        return failed(kFailure, planned.error());
    }
    int code = kOk;
    for (const std::string& video : planned->unfetched) {
        (void)std::printf("%s: no snapshot yet: %s reads it\n", video.c_str(),
                          eval_command(flags, "fetch", video).c_str());
        code = kFailure;
    }
    library::Jobs jobs{*database, SystemClock::instance()};
    std::size_t done = 0;
    for (eval::RunEntry& entry : planned->entries) {
        auto job = eval::start_entry(flags.root, *run, entry, runs, jobs);
        if (!job.has_value()) {
            return failed(kFailure, job.error());
        }
        if (job->status == library::JobStatus::done) {
            ++done;  // done in an earlier run of the command
            continue;
        }
        (void)std::fprintf(stderr, "run %s: %s, replicate %d of %d\n", run->spec.label.c_str(),
                           entry.video.c_str(), entry.replicate + 1, run->spec.replicates);
        const int result = run_job(*hub, *database, *std::move(job), signals);
        if (result == kOk) {
            ++done;
            continue;
        }
        code = result;
        if (signals.received() != 0) {
            break;  // interrupted: the same command takes the run up
        }
    }
    (void)std::printf("run %s: %zu of %zu jobs done\n", run->spec.label.c_str(), done,
                      planned->entries.size());
    if (done == planned->entries.size() && planned->unfetched.empty()) {
        (void)std::printf("  then: %s\n",
                          eval_command(flags, "score", std::format("--label {}", flags.label)).c_str());
    } else {
        (void)std::printf("  take it up again with: %s\n", resume_run_command(flags).c_str());
    }
    return code;
}

int eval_score(const Global& global, const EvalFlags& flags) {
    auto hub = load(global);
    if (!hub.has_value()) {
        return failed(kBadConfiguration, hub.error());
    }
    auto set = eval_set(flags);
    if (!set.has_value()) {
        return failed(kBadConfiguration, set.error());
    }
    auto database = library::open_database(hub->settings.library_database());
    if (!database.has_value()) {
        return failed(kFailure, database.error());
    }
    eval::Runs runs{*database, SystemClock::instance()};
    auto run = runs.find(flags.label);
    if (!run.has_value()) {
        return failed(kFailure, run.error());
    }
    auto scoring = eval::score_run(flags.root, *set, *run, runs, *database, SystemClock::instance());
    if (!scoring.has_value()) {
        return failed(kFailure, scoring.error());
    }
    for (const std::string& note : scoring->notes) {
        (void)std::printf("%s\n", note.c_str());
    }
    for (const std::string& waiting : scoring->waiting) {
        (void)std::printf("not scored: %s\n", waiting.c_str());
    }
    (void)std::printf("run %s: %zu jobs scored, by %s\n", run->spec.label.c_str(), scoring->scored,
                      std::string{eval::kCodeScorer}.c_str());
    if (!scoring->waiting.empty()) {
        (void)std::printf("  take the run up again with: %s\n",
                          eval_command(flags, "run", std::format("--label {}", flags.label)).c_str());
        return kFailure;
    }
    return kOk;
}

int eval_export(const Global& global, const EvalFlags& flags) {
    if (flags.format != "csv" && flags.format != "json") {
        return failed(
            kBadConfiguration,
            Error{Errc::invalid_argument, std::format("--format is csv or json, not '{}'", flags.format)});
    }
    auto hub = load(global);
    if (!hub.has_value()) {
        return failed(kBadConfiguration, hub.error());
    }
    auto set = eval_set(flags);
    if (!set.has_value()) {
        return failed(kBadConfiguration, set.error());
    }
    auto database = library::open_database(hub->settings.library_database());
    if (!database.has_value()) {
        return failed(kFailure, database.error());
    }
    const eval::Runs runs{*database, SystemClock::instance()};
    auto run = runs.find(flags.label);
    if (!run.has_value()) {
        return failed(kFailure, run.error());
    }
    auto scores = runs.scores(run->id);
    if (!scores.has_value()) {
        return failed(kFailure, scores.error());
    }
    const std::string text = flags.format == "csv"
                                 ? eval::to_csv(*run, *set, *scores)
                                 : json::dump_pretty(eval::to_json(*run, *set, *scores)) + "\n";
    if (!flags.out.has_value()) {
        (void)std::fputs(text.c_str(), stdout);
        return kOk;
    }
    if (auto written = write_file_atomically(*flags.out, text); !written.has_value()) {
        return failed(kFailure, written.error());
    }
    (void)std::printf("wrote %s\n", flags.out->string().c_str());
    return kOk;
}

// The facts of `video`'s gold where a person reviewed it for the snapshot
// kept; none otherwise, and `notes` says why.
Result<std::vector<eval::Fact>> reviewed_facts(const EvalFlags& flags, const eval::EvalSet& set,
                                               std::string_view video, std::vector<std::string>& notes) {
    const eval::EvalVideo* found = set.find(video);
    if (found == nullptr) {
        notes.push_back(std::format("{}: the set has it no more: judged without gold", video));
        return std::vector<eval::Fact>{};
    }
    RR_TRY_ASSIGN(const std::optional<eval::Gold> gold, eval::reviewed_gold(flags.root, *found, notes));
    return gold.has_value() ? gold->facts : std::vector<eval::Fact>{};
}

// What judging a run's entries needs, read once (`rreport eval judge`).
struct RunJudging {
    const EvalFlags* flags = nullptr;
    const eval::EvalSet* set = nullptr;
    const Hub* hub = nullptr;
    db::Database* database = nullptr;
    const prompt::Library* prompts = nullptr;
    const eval::Panel* panel = nullptr;
    eval::Runs* runs = nullptr;
    const eval::EvalRun* run = nullptr;
    const proc::TerminationSignals* signals = nullptr;
    std::string again{};                                                  // takes the run up again
    std::map<std::string, std::vector<eval::Fact>, std::less<>> facts{};  // a video's, read once
    std::vector<std::string> notes{};
};

// How judging one entry went: report_run()'s exit code, or kFailure for an
// article job not done; and whether its scores were kept.
struct EntryJudged {
    int code = kOk;
    bool scored = false;
};

// Takes `job`, the judgement of `article`, through its stage, raising the
// army only if the stage asks for it (as run_job() does an article job).
//
// clang-analyzer follows the std::function that video_length_of() makes as
// far as its allocation, but not to its release, which libstdc++ makes
// through a function pointer when the army is destroyed: it reports a leak
// that is none. ASan's leak checker, on every test that judges, finds none.
// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
Result<eval::JudgementRun> judge_job(const RunJudging& judging, const library::Job& article, library::Job job,
                                     eval::ArticleToJudge to_judge) {
    const HubSettings& settings = judging.hub->settings;
    db::Database& database = *judging.database;
    const net::HttpClient http = make_http();
    JobArmy forces{JobArmy::Inputs{.settings = &settings,
                                   .http = &http,
                                   .database = &database,
                                   .clock = &SystemClock::instance(),
                                   .job_id = job.id,
                                   .elapsed_before = job.elapsed,
                                   .video_length = video_length_of(database, article.id),
                                   .stop = judging.signals->stop_token(),
                                   .progress = say}};
    pipeline::StageContext context{.prompts = judging.prompts,
                                   .work_directory = settings.library_database().parent_path() / "jobs",
                                   .stop = judging.signals->stop_token(),
                                   .progress = say};
    if (settings.llm_mode == LlmMode::replay || settings.runtime.has_value()) {
        context.army = [&forces] { return forces.raise(); };
    }
    return eval::run_judgement(std::move(job), *judging.panel, std::move(to_judge), database,
                               SystemClock::instance(), std::move(context));
}
// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)

// One entry of the run: its article job's judgement started, or taken up,
// run, and its scores kept once it is done.
Result<EntryJudged> judge_entry(RunJudging& judging, const eval::RunEntry& entry) {
    db::Database& database = *judging.database;
    const library::Jobs jobs{database, SystemClock::instance()};
    RR_TRY_ASSIGN(const library::Job article, jobs.find(entry.job));
    if (article.status != library::JobStatus::done) {
        (void)std::printf("not judged: %s, replicate %d: job %s is %s\n", entry.video.c_str(),
                          entry.replicate, article.id.c_str(),
                          std::string{library::to_string(article.status)}.c_str());
        return EntryJudged{.code = kFailure};
    }
    auto known = judging.facts.find(entry.video);
    if (known == judging.facts.end()) {
        RR_TRY_ASSIGN(std::vector<eval::Fact> read,
                      reviewed_facts(*judging.flags, *judging.set, entry.video, judging.notes));
        known = judging.facts.emplace(entry.video, std::move(read)).first;
    }
    const library::StageStore store{database, SystemClock::instance()};
    RR_TRY_ASSIGN(eval::ArticleToJudge to_judge, eval::article_to_judge(article, store, known->second));
    RR_TRY_ASSIGN(library::Job job,
                  eval::start_judgement(database, SystemClock::instance(), article, *judging.panel));
    (void)std::fprintf(stderr, "judge %s: %s, replicate %d\n", judging.run->spec.label.c_str(),
                       entry.video.c_str(), entry.replicate + 1);
    RR_TRY_ASSIGN(eval::JudgementRun done, judge_job(judging, article, std::move(job), std::move(to_judge)));
    const int code = report_run(done.run, judging.again, *judging.signals);
    if (!done.scoring.has_value()) {
        return EntryJudged{.code = code};
    }
    RR_TRY(judging.runs->keep(judging.run->id, entry, *done.scoring));
    return EntryJudged{.code = code, .scored = true};
}

int eval_judge(const Global& global, const EvalFlags& flags, const proc::TerminationSignals& signals) {
    auto hub = load(global);
    if (!hub.has_value()) {
        return failed(kBadConfiguration, hub.error());
    }
    auto set = eval_set(flags);
    if (!set.has_value()) {
        return failed(kBadConfiguration, set.error());
    }
    auto panel = eval::read_panel(flags.root, flags.panel);
    if (!panel.has_value()) {
        return failed(kBadConfiguration, panel.error());
    }
    auto prompts = prompt::Library::embedded();
    if (!prompts.has_value()) {
        return failed(kFailure, prompts.error());
    }
    if (auto frozen = eval::check_panel(*panel, *prompts); !frozen.has_value()) {
        return failed(kBadConfiguration, frozen.error());
    }
    auto database = library::open_database(hub->settings.library_database());
    if (!database.has_value()) {
        return failed(kFailure, database.error());
    }
    eval::Runs runs{*database, SystemClock::instance()};
    auto run = runs.find(flags.label);
    if (!run.has_value()) {
        return failed(kFailure, run.error());
    }
    auto entries = runs.entries(run->id);
    if (!entries.has_value()) {
        return failed(kFailure, entries.error());
    }
    RunJudging judging{.flags = &flags,
                       .set = &*set,
                       .hub = &*hub,
                       .database = &*database,
                       .prompts = &*prompts,
                       .panel = &*panel,
                       .runs = &runs,
                       .run = &*run,
                       .signals = &signals,
                       .again = eval_command(flags, "judge",
                                             std::format("--label {} --panel {}", flags.label, flags.panel))};
    std::size_t judged = 0;
    int code = kOk;
    for (const eval::RunEntry& entry : *entries) {
        auto one = judge_entry(judging, entry);
        if (!one.has_value()) {
            return failed(kFailure, one.error());
        }
        if (one->scored) {
            ++judged;
        }
        if (one->code != kOk) {
            code = one->code;
            if (signals.received() != 0) {
                break;  // interrupted: the same command takes the run up
            }
        }
    }
    for (const std::string& note : judging.notes) {
        (void)std::printf("%s\n", note.c_str());
    }
    (void)std::printf("run %s: %zu of %zu jobs judged, by %s%s\n", run->spec.label.c_str(), judged,
                      entries->size(), panel->scorer().c_str(),
                      panel->calibrated ? "" : " (reported only: no person has calibrated the panel yet)");
    if (judged < entries->size()) {
        (void)std::printf("  take it up again with: %s\n", judging.again.c_str());
    }
    return code;
}

// "base (m2 dev, tier max, replicates 0 to 2)"
std::string described(const eval::EvalRun& run) {
    const eval::RunSpec& spec = run.spec;
    const int last = spec.first_replicate + spec.replicates - 1;
    return std::format("{} ({} {}, tier {}, {})", spec.label, spec.eval_set, eval::to_string(spec.split),
                       spec.tier,
                       spec.replicates == 1 ? std::format("replicate {}", last)
                                            : std::format("replicates {} to {}", spec.first_replicate, last));
}

// The two runs `eval compare` and `eval gate` name, and their scores.
struct RunPair {
    eval::EvalRun a{};
    eval::EvalRun b{};
    std::vector<eval::ScoredEntry> a_scores{};
    std::vector<eval::ScoredEntry> b_scores{};
};

Result<RunPair> run_pair(const HubSettings& settings, const EvalFlags& flags) {
    RR_TRY_ASSIGN(db::Database database, library::open_database(settings.library_database()));
    const eval::Runs runs{database, SystemClock::instance()};
    RunPair pair;
    RR_TRY_ASSIGN(pair.a, runs.find(flags.baseline));
    RR_TRY_ASSIGN(pair.b, runs.find(flags.candidate));
    RR_TRY_ASSIGN(pair.a_scores, runs.scores(pair.a.id));
    RR_TRY_ASSIGN(pair.b_scores, runs.scores(pair.b.id));
    return pair;
}

// A run that is not there was asked wrong; a library that cannot be read failed.
int pair_failed(const Error& error) {
    return failed(error.code() == Errc::not_found ? kBadConfiguration : kFailure, error);
}

// What reading two runs' flow needs, read once (`rreport eval compare --flow`).
struct FlowReading {
    const Hub* hub = nullptr;
    db::Database* database = nullptr;
    const prompt::Library* prompts = nullptr;
    const eval::Panel* panel = nullptr;
    const proc::TerminationSignals* signals = nullptr;
    std::string again{};  // takes the pairings up again
    std::vector<std::string> notes{};
    int code = kOk;  // report_run()'s, or kFailure for a pair not made
};

// Takes `job`, the pairing of article job `a` with another, through its
// stage, raising the army only if the stage asks for it (as run_job() does
// an article job).
//
// clang-analyzer follows the std::function that video_length_of() makes as
// far as its allocation, but not to its release, which libstdc++ makes
// through a function pointer when the army is destroyed: it reports a leak
// that is none. ASan's leak checker, on every test that pairs, finds none.
// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
Result<eval::PairingRun> pair_job(const FlowReading& reading, const library::Job& a, library::Job job,
                                  eval::ArticlesToCompare articles) {
    const HubSettings& settings = reading.hub->settings;
    db::Database& database = *reading.database;
    const net::HttpClient http = make_http();
    JobArmy forces{JobArmy::Inputs{.settings = &settings,
                                   .http = &http,
                                   .database = &database,
                                   .clock = &SystemClock::instance(),
                                   .job_id = job.id,
                                   .elapsed_before = job.elapsed,
                                   .video_length = video_length_of(database, a.id),
                                   .stop = reading.signals->stop_token(),
                                   .progress = say}};
    pipeline::StageContext context{.prompts = reading.prompts,
                                   .work_directory = settings.library_database().parent_path() / "jobs",
                                   .stop = reading.signals->stop_token(),
                                   .progress = say};
    if (settings.llm_mode == LlmMode::replay || settings.runtime.has_value()) {
        context.army = [&forces] { return forces.raise(); };
    }
    return eval::run_pairing(std::move(job), *reading.panel, std::move(articles), database,
                             SystemClock::instance(), std::move(context));
}
// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)

// B's preference for one pair of entries, A's and B's of one video, once
// their pairing is done; none otherwise, and `reading` says why.
Result<std::optional<double>> read_pair(FlowReading& reading, const eval::RunEntry& a_entry,
                                        const eval::RunEntry& b_entry) {
    db::Database& database = *reading.database;
    const auto unpaired = [&](std::string_view why) {
        reading.notes.push_back(std::format("not paired: {}, replicates {} and {}: {}", a_entry.video,
                                            a_entry.replicate, b_entry.replicate, why));
        reading.code = kFailure;
        return std::optional<double>{};
    };
    const library::Jobs jobs{database, SystemClock::instance()};
    RR_TRY_ASSIGN(const library::Job a, jobs.find(a_entry.job));
    RR_TRY_ASSIGN(const library::Job b, jobs.find(b_entry.job));
    for (const library::Job* job : {&a, &b}) {
        if (job->status != library::JobStatus::done) {
            return unpaired(std::format("job {} is {}", job->id, library::to_string(job->status)));
        }
    }
    const library::StageStore store{database, SystemClock::instance()};
    auto articles = eval::articles_to_compare(a, b, store);
    if (!articles.has_value()) {
        if (articles.error().code() != Errc::invalid_argument) {
            return fail(std::move(articles).error());
        }
        return unpaired(articles.error().message());  // no article, or two videos' transcripts
    }
    RR_TRY_ASSIGN(library::Job job,
                  eval::start_pairing(database, SystemClock::instance(), a, b, *reading.panel));
    (void)std::fprintf(stderr, "flow: %s, replicates %d and %d\n", a_entry.video.c_str(), a_entry.replicate,
                       b_entry.replicate);
    RR_TRY_ASSIGN(const eval::PairingRun done, pair_job(reading, a, std::move(job), *std::move(articles)));
    if (const int code = report_run(done.run, reading.again, *reading.signals); code != kOk) {
        reading.code = code;
    }
    if (!done.score.has_value()) {
        return std::optional<double>{};  // not done: report_run() said why
    }
    if (!done.score->b_preferred.has_value()) {
        reading.notes.push_back(std::format("{}, replicates {} and {}: no part both articles tell was read",
                                            a_entry.video, a_entry.replicate, b_entry.replicate));
    }
    return done.score->b_preferred;
}

// Each video's B preferences, one a pair of its entries read
// (eval::pairs_to_read()). Stops at a signal.
Result<std::map<std::string, std::vector<double>, std::less<>>> read_flow(FlowReading& reading,
                                                                          const RunPair& pair) {
    std::map<std::string, std::vector<double>, std::less<>> read;
    for (const eval::VideoPairs& video : eval::pairs_to_read(pair.a_scores, pair.b_scores)) {
        if (video.a_replicates != video.b_replicates) {
            reading.notes.push_back(
                std::format("{}: {} replicates in A and {} in B: the first {} of each paired", video.video,
                            video.a_replicates, video.b_replicates, video.pairs.size()));
        }
        std::vector<double>& values = read[video.video];
        for (const auto& [a_entry, b_entry] : video.pairs) {
            RR_TRY_ASSIGN(const std::optional<double> preferred, read_pair(reading, a_entry, b_entry));
            if (reading.signals->received() != 0) {
                return read;  // interrupted: the same command takes the pairings up
            }
            if (preferred.has_value()) {
                values.push_back(*preferred);
            }
        }
    }
    return read;
}

// The flow row of `eval compare --flow`, by the panel the flags name; none
// when it could not be read at all, and `code` is the exit code.
struct FlowRow {
    std::optional<eval::Comparison> comparison{};
    std::vector<std::string> notes{};
    int code = kOk;
};

FlowRow flow_row(const Hub& hub, const EvalFlags& flags, const RunPair& pair,
                 const proc::TerminationSignals& signals) {
    auto panel = eval::read_panel(flags.root, flags.panel);
    if (!panel.has_value()) {
        return FlowRow{.code = failed(kBadConfiguration, panel.error())};
    }
    auto prompts = prompt::Library::embedded();
    if (!prompts.has_value()) {
        return FlowRow{.code = failed(kFailure, prompts.error())};
    }
    if (auto frozen = eval::check_panel(*panel, *prompts); !frozen.has_value()) {
        return FlowRow{.code = failed(kBadConfiguration, frozen.error())};
    }
    auto database = library::open_database(hub.settings.library_database());
    if (!database.has_value()) {
        return FlowRow{.code = failed(kFailure, database.error())};
    }
    FlowReading reading{.hub = &hub,
                        .database = &*database,
                        .prompts = &*prompts,
                        .panel = &*panel,
                        .signals = &signals,
                        .again = eval_command(flags, "compare",
                                              std::format("{} {} --flow --panel {}", flags.baseline,
                                                          flags.candidate, flags.panel))};
    auto read = read_flow(reading, pair);
    if (!read.has_value()) {
        return FlowRow{.code = failed(kFailure, read.error())};
    }
    if (signals.received() != 0) {
        return FlowRow{.code = reading.code};  // report_run() said how to take it up
    }
    return FlowRow{.comparison = eval::flow_comparison(*panel, *read),
                   .notes = std::move(reading.notes),
                   .code = reading.code};
}

int eval_compare(const Global& global, const EvalFlags& flags, const proc::TerminationSignals& signals) {
    auto hub = load(global);
    if (!hub.has_value()) {
        return failed(kBadConfiguration, hub.error());
    }
    auto gate = eval::read_gate(flags.root, flags.gate);
    if (!gate.has_value()) {
        return failed(kBadConfiguration, gate.error());
    }
    const eval::Metric* one = flags.metric.has_value() ? eval::find_metric(*flags.metric) : nullptr;
    if (one != nullptr && one->pairwise && !flags.flow) {
        return failed(
            kBadConfiguration,
            Error{Errc::invalid_argument,
                  std::format("{} is read pairwise, between the two runs' articles: add --flow, and "
                              "the panel reads them",
                              one->name)});
    }
    auto pair = run_pair(hub->settings, flags);
    if (!pair.has_value()) {
        return pair_failed(pair.error());
    }
    FlowRow flow;
    if (flags.flow) {
        flow = flow_row(*hub, flags, *pair, signals);
        if (!flow.comparison.has_value()) {
            return flow.code;
        }
    }
    std::string text;
    if (one != nullptr && one->pairwise && flow.comparison.has_value()) {
        text = eval::by_video_text(described(pair->a), described(pair->b), *flow.comparison);
    } else if (flags.metric.has_value()) {
        auto by_video = eval::by_video_text(described(pair->a), described(pair->b), pair->a_scores,
                                            pair->b_scores, *flags.metric);
        if (!by_video.has_value()) {
            return failed(kBadConfiguration, by_video.error());
        }
        text = *std::move(by_video);
    } else {
        std::vector<eval::Comparison> read_pairwise;
        if (flow.comparison.has_value()) {
            read_pairwise.push_back(*flow.comparison);
        }
        text = eval::to_text(described(pair->a), described(pair->b),
                             eval::compare_runs(pair->a_scores, pair->b_scores, gate->gated, read_pairwise));
    }
    if (!flow.notes.empty()) {
        text += "\nwhat the flow could not read:\n";
        for (const std::string& note : flow.notes) {
            text += std::format("  {}\n", note);
        }
    }
    (void)std::fputs(text.c_str(), stdout);
    return flow.code;
}

int eval_gate(const Global& global, const EvalFlags& flags) {
    auto hub = load(global);
    if (!hub.has_value()) {
        return failed(kBadConfiguration, hub.error());
    }
    auto gate = eval::read_gate(flags.root, flags.gate);
    if (!gate.has_value()) {
        return failed(kBadConfiguration, gate.error());
    }
    auto panel = eval::read_panel(flags.root, flags.panel);
    if (!panel.has_value()) {
        return failed(kBadConfiguration, panel.error());
    }
    auto pair = run_pair(hub->settings, flags);
    if (!pair.has_value()) {
        return pair_failed(pair.error());
    }
    if (flags.aa) {
        if (auto again = eval::check_aa(pair->a, pair->b); !again.has_value()) {
            return failed(kBadConfiguration, again.error());
        }
        const eval::AaReading reading = eval::read_gate_on_aa(*gate, pair->a_scores, pair->b_scores, *panel);
        (void)std::fputs(eval::to_text(*gate, reading, described(pair->a), described(pair->b)).c_str(),
                         stdout);
        return reading.holds() ? kOk : kFailure;
    }
    const eval::GateReading reading = eval::read_gate_on(*gate, pair->a_scores, pair->b_scores, *panel);
    (void)std::fputs(eval::to_text(*gate, reading, described(pair->a), described(pair->b)).c_str(), stdout);
    return reading.passed() ? kOk : kFailure;
}

// One run of a sweep, as the eval's own commands make it: run, scored, and
// judged unless asked not to. The worst of their exit codes.
int sweep_run(const Global& global, const EvalFlags& flags, const proc::TerminationSignals& signals) {
    (void)std::printf("\n== %s: tier %s\n", flags.label.c_str(), flags.tier.c_str());
    const int ran = eval_run(global, flags, signals);
    if (ran == kBadConfiguration || signals.received() != 0) {
        return ran;  // a label of another run, or interrupted: nothing after it would hold
    }
    const int scored = eval_score(global, flags);
    if (flags.no_judge) {
        return std::max(ran, scored);
    }
    return std::max({ran, scored, eval_judge(global, flags, signals)});
}

// What the sweep's runs found: each variant against the baseline.
Result<std::vector<eval::VariantFound>> sweep_found(const HubSettings& settings, const eval::Sweep& sweep) {
    RR_TRY_ASSIGN(db::Database database, library::open_database(settings.library_database()));
    const eval::Runs runs{database, SystemClock::instance()};
    RR_TRY_ASSIGN(const eval::EvalRun baseline, runs.find(sweep.baseline_label()));
    RR_TRY_ASSIGN(const std::vector<eval::ScoredEntry> baseline_scores, runs.scores(baseline.id));
    std::vector<eval::VariantFound> found;
    for (const eval::SweepVariant& variant : sweep.variants) {
        RR_TRY_ASSIGN(const eval::EvalRun run, runs.find(sweep.label_of(variant)));
        RR_TRY_ASSIGN(const std::vector<eval::ScoredEntry> scores, runs.scores(run.id));
        found.push_back(
            eval::VariantFound{.variant = variant, .rows = eval::compare_runs(baseline_scores, scores)});
    }
    return found;
}

int eval_sweep(const Global& global, const EvalFlags& flags, const proc::TerminationSignals& signals) {
    auto hub = load(global);
    if (!hub.has_value()) {
        return failed(kBadConfiguration, hub.error());
    }
    auto sweep = eval::read_sweep(flags.root, flags.sweep);
    if (!sweep.has_value()) {
        return failed(kBadConfiguration, sweep.error());
    }
    std::vector<std::pair<std::string, std::string>> labelled{{sweep->baseline_label(), sweep->baseline}};
    for (const eval::SweepVariant& variant : sweep->variants) {
        labelled.emplace_back(sweep->label_of(variant), variant.tier);
    }
    int code = kOk;
    for (const auto& [label, tier] : labelled) {
        EvalFlags one = flags;
        one.label = label;
        one.tier = tier;
        one.replicates = sweep->replicates;
        one.first_replicate = 0;  // every run under the same salts
        one.split = std::string{eval::to_string(eval::Split::dev)};
        one.videos.clear();
        const int done = sweep_run(global, one, signals);
        if (done == kBadConfiguration || signals.received() != 0) {
            return done;
        }
        code = std::max(code, done);
    }
    auto found = sweep_found(hub->settings, *sweep);
    if (!found.has_value()) {
        return failed(kFailure, found.error());
    }
    (void)std::printf("\n%s", eval::to_text(*sweep, *found).c_str());
    return code;
}

int eval_report(const Global& global, const EvalFlags& flags) {
    auto hub = load(global);
    if (!hub.has_value()) {
        return failed(kBadConfiguration, hub.error());
    }
    if (flags.metric.has_value()) {
        const eval::Metric* metric = eval::find_metric(*flags.metric);
        if (metric == nullptr || metric->pairwise) {
            return failed(kBadConfiguration,
                          Error{Errc::invalid_argument,
                                std::format("'{}' is no metric a run scores: docs/design/eval.md lists them",
                                            *flags.metric)});
        }
    }
    auto database = library::open_database(hub->settings.library_database());
    if (!database.has_value()) {
        return failed(kFailure, database.error());
    }
    const eval::Runs runs{*database, SystemClock::instance()};
    auto all = runs.all();
    if (!all.has_value()) {
        return failed(kFailure, all.error());
    }
    std::vector<eval::RunScores> scored;
    scored.reserve(all->size());
    for (eval::EvalRun& run : *all) {
        auto scores = runs.scores(run.id);
        if (!scores.has_value()) {
            return failed(kFailure, scores.error());
        }
        scored.push_back(eval::RunScores{.run = std::move(run), .scores = *std::move(scores)});
    }
    const eval::PageOptions options{.quality = flags.metric.value_or(eval::default_quality(scored)),
                                    .made = when(std::chrono::system_clock::now())};
    const std::filesystem::path out = flags.out.value_or(flags.root / "report.html");
    if (auto written = write_file_atomically(out, eval::eval_page(scored, options)); !written.has_value()) {
        return failed(kFailure, written.error());
    }
    (void)std::printf("wrote %s: %zu run%s, %s against %s\n", out.string().c_str(), scored.size(),
                      scored.size() == 1 ? "" : "s", options.quality.c_str(), options.cost.c_str());
    return kOk;
}

}  // namespace

struct EvalCommands::Commands {
    EvalFlags flags;
    std::string sheet_video;
    CLI::App* fetch_command = nullptr;
    CLI::App* annotate_command = nullptr;
    CLI::App* sheet_command = nullptr;
    CLI::App* run_command = nullptr;
    CLI::App* score_command = nullptr;
    CLI::App* export_command = nullptr;
    CLI::App* judge_command = nullptr;
    CLI::App* compare_command = nullptr;
    CLI::App* gate_command = nullptr;
    CLI::App* sweep_command = nullptr;
    CLI::App* report_command = nullptr;
};

EvalCommands::EvalCommands(CLI::App& app) : commands_(std::make_unique<Commands>()) {
    Commands& commands = *commands_;
    EvalFlags& flags = commands.flags;
    auto* group =
        app.add_subcommand("eval", "The evaluation: its videos, gold, runs, scores and judges (ADR-0019)")
            ->require_subcommand(1);
    group->add_option("--eval-dir", flags.root, "The eval's directory (default: eval)");
    group->add_option("--eval-set", flags.set, "The set of videos: <eval-dir>/sets/<set>.toml (default: m2)");
    commands.fetch_command =
        group->add_subcommand("fetch", "Read each video once through the source stage, and keep it");
    commands.fetch_command->add_option("videos", flags.videos,
                                       "The videos (default: every video of the set)");
    commands.fetch_command->add_flag("--again", flags.again, "Read a video kept already once more");
    commands.annotate_command =
        group->add_subcommand("annotate", "Draft each video's gold annotation with the pod's models");
    commands.annotate_command->add_option("videos", flags.videos,
                                          "The videos (default: every video of the set)");
    commands.annotate_command->add_flag("--replace", flags.replace,
                                        "Draft a silver gold file again, losing what was corrected in it");
    commands.sheet_command = group->add_subcommand("sheet", "The sheet a person corrects a gold file with");
    commands.sheet_command->add_option("video", commands.sheet_video, "The video")->required();
    commands.sheet_command->add_option("--out", flags.out, "Write it to this file (default: print it)");
    commands.run_command =
        group->add_subcommand("run", "Take the set's videos through the article stages, each replicate");
    commands.run_command->add_option("videos", flags.videos,
                                     "The videos (default: every video of the split)");
    commands.run_command->add_option("--label", flags.label, "The run's name: the same again takes it up")
        ->required();
    commands.run_command->add_option("--tier", flags.tier, "fast, thorough or max (default: max)");
    commands.run_command->add_option("--replicates", flags.replicates,
                                     "How many times each video is taken through (default: 3)");
    commands.run_command->add_option("--split", flags.split, "dev, or test for a gate (default: dev)");
    commands.run_command->add_option("--first-replicate", flags.first_replicate,
                                     "The first replicate's number, its salt: an A/A run starts after its "
                                     "baseline's last (default: 0)");
    commands.score_command =
        group->add_subcommand("score", "Score each done job of a run by code: what it kept, what it cost");
    commands.score_command->add_option("--label", flags.label, "The run")->required();
    commands.export_command =
        group->add_subcommand("export", "A run's scores, a row per video and replicate");
    commands.export_command->add_option("--label", flags.label, "The run")->required();
    commands.export_command->add_option("--format", flags.format, "csv or json (default: csv)");
    commands.export_command->add_option("--out", flags.out, "Write it to this file (default: print it)");
    commands.judge_command = group->add_subcommand(
        "judge",
        "Read each done job of a run with the eval's frozen judges: facts told, sentences supported");
    commands.judge_command->add_option("--label", flags.label, "The run")->required();
    commands.judge_command->add_option("--panel", flags.panel,
                                       "The frozen panel, eval/judges/<panel>.toml (default: eval-v1)");
    commands.compare_command = group->add_subcommand(
        "compare", "Two runs side by side, every metric, video by video, with its 95% interval");
    commands.compare_command->add_option("baseline", flags.baseline, "Run A, the baseline")->required();
    commands.compare_command->add_option("candidate", flags.candidate, "Run B, the candidate")->required();
    commands.compare_command->add_option("--gate", flags.gate,
                                         "Whose margins to show, eval/gates/<gate>.toml (default: default)");
    commands.compare_command->add_option("--metric", flags.metric, "One metric, video by video");
    commands.compare_command->add_flag(
        "--flow", flags.flow, "Read each video's two articles side by side with the frozen judges: flow");
    commands.compare_command->add_option("--panel", flags.panel,
                                         "The frozen panel that reads them, with --flow (default: eval-v1)");
    commands.gate_command = group->add_subcommand(
        "gate", "Whether the candidate may replace the baseline: its invariants and margins (ADR-0019)");
    commands.gate_command->add_option("baseline", flags.baseline, "Run A, the baseline")->required();
    commands.gate_command->add_option("candidate", flags.candidate, "Run B, the candidate")->required();
    commands.gate_command->add_option("--gate", flags.gate, "eval/gates/<gate>.toml (default: default)");
    commands.gate_command->add_option("--panel", flags.panel,
                                      "The panel judged metrics must be scored by (default: eval-v1)");
    commands.gate_command->add_flag(
        "--aa", flags.aa, "B is A run again under other salts: check whether the gate's margins hold");
    commands.sweep_command = group->add_subcommand(
        "sweep", "Each variant of a sweep run, scored and judged, then compared with its baseline");
    commands.sweep_command->add_option("name", flags.sweep, "The sweep: eval/sweeps/<name>.toml")->required();
    commands.sweep_command->add_option("--panel", flags.panel,
                                       "The frozen panel that judges each run (default: eval-v1)");
    commands.sweep_command->add_flag("--no-judge", flags.no_judge, "Score by code alone: no judge is asked");
    commands.report_command = group->add_subcommand(
        "report", "Every eval run on one page: quality against cost, each run's videos");
    commands.report_command->add_option("--out", flags.out,
                                        "Write it here (default: <eval-dir>/report.html)");
    commands.report_command->add_option(
        "--metric", flags.metric,
        "The chart's quality (default: facts_told once a run is judged, else atoms_told)");
}

EvalCommands::~EvalCommands() = default;

std::optional<int> EvalCommands::run(const Global& global, const proc::TerminationSignals& signals) const {
    const Commands& commands = *commands_;
    if (*commands.fetch_command) {
        return eval_fetch(global, commands.flags, signals);
    }
    if (*commands.annotate_command) {
        return eval_annotate(global, commands.flags, signals);
    }
    if (*commands.sheet_command) {
        EvalFlags flags = commands.flags;
        flags.videos = {commands.sheet_video};
        return eval_sheet(flags);
    }
    if (*commands.run_command) {
        return eval_run(global, commands.flags, signals);
    }
    if (*commands.score_command) {
        return eval_score(global, commands.flags);
    }
    if (*commands.export_command) {
        return eval_export(global, commands.flags);
    }
    if (*commands.judge_command) {
        return eval_judge(global, commands.flags, signals);
    }
    if (*commands.compare_command) {
        return eval_compare(global, commands.flags, signals);
    }
    if (*commands.gate_command) {
        return eval_gate(global, commands.flags);
    }
    if (*commands.sweep_command) {
        return eval_sweep(global, commands.flags, signals);
    }
    if (*commands.report_command) {
        return eval_report(global, commands.flags);
    }
    return std::nullopt;
}

}  // namespace rr::cli
