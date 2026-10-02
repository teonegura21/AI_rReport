// The intelligence core end to end, through the real executables (plan P22):
// rreport takes a synthetic lecture through every stage at the max tier,
// against a real rreport-runtime whose fake engines answer as the oracle
// does (rr/oracle/answers.hpp): Qwen the commander, Gemma the infantry, so
// the judge is of another family than the writer.
//
// Then the crash a job must survive (ADR-0020: a job resumes by its memo of
// answers). rreport is killed with SIGKILL partway, and `rreport resume`
// takes the job up. A call whose answer the memo kept is never asked again,
// so on resume each engine answers exactly the calls of a clean run that the
// memo did not keep:
//     its calls on resume == its calls in a clean run − its answers kept,
// and the article is byte for byte the clean run's.
//
// The eval runs on the same engines (plan P23): `rreport eval run` takes the
// lecture's snapshot through every stage once a replicate, `score` scores
// each job by code, and `export` writes the scores; a run asked for again
// asks nothing again.
//
// Where the kill falls: each engine of the killed run answers its first N
// calls, then hangs every later one (--fake-hang-after), and the kill comes
// once one has hung. An oracle answers at once, so a kill timed by the clock
// would often find the job done; a hang holds it at any point, N drawn from
// a seeded generator. Calls may still be in flight on the other engine when
// the kill lands: one whose answer was not yet kept is asked again on
// resume, as the formula counts it.
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/core/clock.hpp>
#include <rr/core/file_io.hpp>
#include <rr/core/hash.hpp>
#include <rr/core/result.hpp>
#include <rr/db/database.hpp>
#include <rr/eval/gold.hpp>
#include <rr/eval/judgement.hpp>
#include <rr/eval/labels.hpp>
#include <rr/eval/panel.hpp>
#include <rr/eval/set.hpp>
#include <rr/eval/snapshot.hpp>
#include <rr/json/json.hpp>
#include <rr/library/library.hpp>
#include <rr/library/stage_store.hpp>
#include <rr/net/http_client.hpp>
#include <rr/pipeline/judging.hpp>
#include <rr/proc/process.hpp>
#include <rr/prompt/library.hpp>
#include <rr/reason/pairwise.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

#include "../unit/pipeline/lecture.hpp"
#include "runtime_process.hpp"

namespace rr {
namespace {

using namespace std::chrono_literals;
using ::testing::HasSubstr;
using ::testing::Not;
using testkit::unwrap;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kCommander = "qwen-commander";
constexpr std::string_view kInfantry = "gemma-infantry";

// The lecture as a caption file (rr/pipeline lecture.hpp): its thirty
// sentences, one every 30 s, each said for 25 s, punctuated as a person
// would, so no model punctuates it.
std::string lecture_captions() {
    const auto stamp = [](int seconds) {
        return std::format("00:{:02}:{:02}.000", seconds / 60, seconds % 60);
    };
    std::string vtt = "WEBVTT\n";
    const std::vector<std::string> texts = pipeline::testing::lecture_texts();
    for (std::size_t index = 0; index < texts.size(); ++index) {
        const int start = static_cast<int>(index) * 30;
        vtt += std::format("\n{} --> {}\n{}\n", stamp(start), stamp(start + 25), texts[index]);
    }
    return vtt;
}

// What each engine has done, as it counts (rr_fake_engine's /stats).
struct EngineCounts {
    std::int64_t completions = 0;
    std::int64_t hung = 0;
};

// How many calls each engine answers before it hangs every later one (none:
// it never hangs).
struct Hang {
    std::optional<int> commander{};
    std::optional<int> infantry{};

    [[nodiscard]] std::string said() const {
        return commander.has_value()
                   ? std::format("the commander hangs after {} calls", *commander)
                   : std::format("the infantry hangs after {} calls", infantry.value_or(-1));
    }
};

// A runtime of two fake engines answering as the oracle: the commander
// first, then the infantry, each hanging as `hang` says.
class OracleRuntime {
public:
    explicit OracleRuntime(Hang hang = {}) : process_(catalog(hang), {"qwen.gguf", "gemma.gguf"}) {}

    [[nodiscard]] std::string url() const { return process_.url(); }
    [[nodiscard]] EngineCounts commander() const { return unwrap(counts(0)); }
    [[nodiscard]] EngineCounts infantry() const { return unwrap(counts(1)); }

    // Calls hung so far, on either engine: none on an engine not yet
    // listening, as when the runtime has only begun to start it.
    [[nodiscard]] std::int64_t hung() const {
        std::int64_t calls = 0;
        for (const int engine : {0, 1}) {
            if (const Result<EngineCounts> read = counts(engine); read.has_value()) {
                calls += read->hung;
            }
        }
        return calls;
    }

private:
    static std::string catalog(const Hang& hang) {
        const auto args = [](std::optional<int> after) {
            std::string list = R"("--fake-oracle", "--fake-token-ms", "0")";
            if (after.has_value()) {
                list += std::format(R"(, "--fake-hang-after", "{}")", *after);
            }
            return list;
        };
        return std::format(R"toml(
[[model]]
id = "{0}"
role = "commander"
file = "qwen.gguf"
sha256 = "{{sha}}"
[[model]]
id = "{1}"
role = "infantry"
file = "gemma.gguf"
sha256 = "{{sha}}"

[[profile]]
name = "test"
  [[profile.engine]]
  model = "{0}"
  slots = 4
  context = 131072
  kv_unified = true
  extra_args = [{2}]
  [[profile.engine]]
  model = "{1}"
  slots = 4
  context = 65536
  kv_unified = true
  extra_args = [{3}]
)toml",
                           kCommander, kInfantry, args(hang.commander), args(hang.infantry));
    }

    [[nodiscard]] Result<EngineCounts> counts(int engine) const {
        const net::HttpClient client;
        RR_TRY_ASSIGN(
            const net::HttpClient::Response stats,
            client.send({.url = std::format("http://127.0.0.1:{}/stats", process_.engine_port(engine))}));
        RR_TRY_ASSIGN(const json::Json read, json::parse(stats.body));
        return EngineCounts{.completions = read.at("completions").get<std::int64_t>(),
                            .hung = read.at("hung").get<std::int64_t>()};
    }

    component::RuntimeProcess process_;
};

// A library of jobs, as rreport keeps one in its data directory.
class Library {
public:
    explicit Library(std::filesystem::path data_dir) : data_dir_(std::move(data_dir)) {}

    [[nodiscard]] const std::filesystem::path& data_dir() const { return data_dir_; }

    // The answers `job`'s memo keeps, by the model that gave them.
    [[nodiscard]] std::map<std::string, std::int64_t> kept(const std::string& job) const {
        db::Database database = open();
        std::map<std::string, std::int64_t> by_model;
        unwrap(database.read([&](db::ReadTx& tx) {
            return tx.query("SELECT response FROM llm_memo WHERE job_id = ?1", {job},
                            [&](const db::Row& row) -> Result<void> {
                                RR_TRY_ASSIGN(const json::Json response, json::parse(row.get_text(0)));
                                ++by_model[response.value("model", "")];
                                return ok();
                            });
        }));
        return by_model;
    }

    // The missions `job`'s battle log keeps (migrations/0003_missions.sql),
    // each with the samples it asked for.
    [[nodiscard]] std::map<std::string, std::int64_t> missions(const std::string& job) const {
        db::Database database = open();
        std::map<std::string, std::int64_t> samples;
        unwrap(database.read([&](db::ReadTx& tx) {
            return tx.query("SELECT mission, samples FROM missions WHERE job_id = ?1", {job},
                            [&](const db::Row& row) -> Result<void> {
                                samples.emplace(row.get_text(0), row.get_int(1));
                                return ok();
                            });
        }));
        return samples;
    }

    // The hash of the article `job`'s seams stage made: equal hashes, equal
    // artifacts, byte for byte (ADR-0004).
    [[nodiscard]] std::string article(const std::string& job) const {
        db::Database database = open();
        const SystemClock& clock = SystemClock::instance();
        const library::StageStore store{database, clock};
        for (const library::StageRun& run : unwrap(store.runs(job))) {
            if (run.stage == "seams" && run.artifact.has_value()) {
                return *run.artifact;
            }
        }
        return {};
    }

private:
    [[nodiscard]] db::Database open() const {
        return unwrap(library::open_database(data_dir_ / "libraries" / "default" / "library.db"));
    }

    std::filesystem::path data_dir_;
};

class EndToEnd : public ::testing::Test {
protected:
    EndToEnd() { captions_ = home_.write_file("Weighing radium.vtt", lecture_captions()); }

    // `rreport <command…>` on `library`, against `runtime`: its command and
    // the settings and environment it is given.
    [[nodiscard]] proc::Command rreport(const std::vector<std::string>& command, const Library& library,
                                        const OracleRuntime& runtime) const {
        std::vector<std::string> argv{RR_HUB_BINARY};
        argv.insert(argv.end(), command.begin(), command.end());
        for (const std::string& setting : {std::format("hub.data_dir={}", library.data_dir().string()),
                                           std::format("runtime.url={}", runtime.url()),
                                           std::string{"runpod.pod_id="}, std::string{"log.level=warn"}}) {
            argv.emplace_back("--set");
            argv.push_back(setting);
        }
        return proc::Command{
            .argv = std::move(argv),
            .environment = {{"HOME", home_.path().string()},
                            {"RREPORT_RUNTIME__TOKEN", std::string{component::kRuntimeToken}},
                            {"PATH", "/usr/bin:/bin"}},
            .inherit_environment = false};
    }

    [[nodiscard]] proc::RunResult run(const std::vector<std::string>& command, const Library& library,
                                      const OracleRuntime& runtime) const {
        return unwrap(proc::run(rreport(command, library, runtime), proc::RunOptions{.timeout = 300s}));
    }

    [[nodiscard]] const std::filesystem::path& captions() const { return captions_; }
    [[nodiscard]] const testkit::TempDir& home() const { return home_; }
    [[nodiscard]] Library library(std::string_view name) const { return Library{home_ / std::string{name}}; }

private:
    testkit::TempDir home_{"rr_end_to_end"};
    std::filesystem::path captions_;
};

// The job a run of `rreport ingest` names first, on stderr: "job <id>: …".
std::string job_in(const std::string& output) {
    const std::size_t at = output.find("job ");
    return at == std::string::npos ? std::string{} : output.substr(at + 4, 26);
}

// The Fidelity Report's table of what was kept (rr/report/page.hpp).
std::string kept_table(const std::string& page) {
    const std::size_t begin = page.find(R"(<table class="coverage">)");
    const std::size_t end = page.find("</table>", begin);
    return begin == std::string::npos || end == std::string::npos ? std::string{}
                                                                  : page.substr(begin, end - begin);
}

// What a clean run asked, and made.
struct CleanRun {
    std::int64_t commander = 0;  // calls each engine answered
    std::int64_t infantry = 0;
    std::string article{};  // its hash
};

TEST_F(EndToEnd, AJobKilledAnywhereResumesAskingOnlyWhatItsMemoDidNotKeep) {
    // A clean run: every stage, both engines, an article that tells every
    // atom, and a memo forgotten once the job is done.
    CleanRun clean;
    {
        const OracleRuntime runtime;
        const Library library = this->library("clean");
        const proc::RunResult ingested =
            run({"ingest", captions().string(), "--tier", "max"}, library, runtime);
        ASSERT_EQ(ingested.status.code, 0) << ingested.stdout_text << ingested.stderr_text;
        const std::string job = job_in(ingested.stderr_text);
        clean = CleanRun{.commander = runtime.commander().completions,
                         .infantry = runtime.infantry().completions,
                         .article = library.article(job)};
        EXPECT_GT(clean.commander, 0);
        EXPECT_GT(clean.infantry, 0);
        EXPECT_FALSE(clean.article.empty()) << "the seams stage made the article";
        EXPECT_TRUE(library.kept(job).empty()) << "a job done forgets its memo";

        const proc::RunResult reported = run({"report", job}, library, runtime);
        ASSERT_EQ(reported.status.code, 0) << reported.stderr_text;
        const std::string page =
            unwrap(read_file(reported.stdout_text.substr(0, reported.stdout_text.find('\n'))));
        const std::string table = kept_table(page);
        EXPECT_THAT(table, HasSubstr("<tr><td>Sentences accounted for</td><td>100.0%</td>"));
        EXPECT_THAT(table, HasSubstr("<tr><td>Atoms told</td><td>100.0%</td>"));
        EXPECT_THAT(table, HasSubstr("<tr><td>Invalid markers</td><td>0</td>"));
    }

    // Kill points: one engine hangs, the other answers all it is asked. Each
    // engine's first call and its last, then two drawn from a seeded
    // generator, a different engine and point each time it can.
    const int commander_calls = static_cast<int>(clean.commander);
    const int infantry_calls = static_cast<int>(clean.infantry);
    std::vector<Hang> kills{{.commander = 0},
                            {.infantry = 0},
                            {.commander = commander_calls - 1},
                            {.infantry = infantry_calls - 1}};
    std::mt19937 draw{20'261'002};
    for (const bool commander : {true, false}) {
        std::uniform_int_distribution<int> call(1, (commander ? commander_calls : infantry_calls) - 2);
        kills.push_back(commander ? Hang{.commander = call(draw)} : Hang{.infantry = call(draw)});
    }

    std::int64_t kept_at_all = 0;
    for (std::size_t point = 0; point < kills.size(); ++point) {
        const Hang& hang = kills[point];
        SCOPED_TRACE(std::format("killed when {}", hang.said()));
        const Library library = this->library(std::format("killed-{}", point));
        std::string job;
        {
            const OracleRuntime hanging{hang};
            std::string output;
            std::mutex output_mutex;
            const proc::ChildProcess hub = unwrap(proc::ChildProcess::start(
                rreport({"ingest", captions().string(), "--tier", "max"}, library, hanging),
                proc::ChildProcess::Options{.on_output = [&](std::string_view line) {
                    const std::lock_guard lock(output_mutex);
                    output += std::string{line} + "\n";
                }}));
            const auto deadline = Clock::now() + 120s;
            while (hanging.hung() == 0 && hub.running() && Clock::now() < deadline) {
                std::this_thread::sleep_for(10ms);
            }
            ASSERT_TRUE(hub.running()) << "the job ended before an engine hung: " << output;
            ASSERT_LT(Clock::now(), deadline) << "no engine hung: " << output;
            ASSERT_EQ(::kill(hub.pid(), SIGKILL), 0);
            (void)hub.wait(10s);
            const std::lock_guard lock(output_mutex);
            job = job_in(output);
        }
        ASSERT_FALSE(job.empty()) << "the job was named before it was killed";
        std::map<std::string, std::int64_t> kept = library.kept(job);
        const std::int64_t kept_commander = kept[std::string{kCommander}];
        const std::int64_t kept_infantry = kept[std::string{kInfantry}];
        EXPECT_EQ(kept.size(), 2U) << "every answer kept is one of the two engines'";
        kept_at_all += kept_commander + kept_infantry;
        EXPECT_LE(kept_commander, hang.commander.value_or(commander_calls)) << "no more kept than answered";
        EXPECT_LE(kept_infantry, hang.infantry.value_or(infantry_calls)) << "no more kept than answered";

        const OracleRuntime resumed_on;
        const proc::RunResult resumed = run({"resume", job}, library, resumed_on);
        ASSERT_EQ(resumed.status.code, 0) << resumed.stdout_text << resumed.stderr_text;
        EXPECT_EQ(resumed_on.commander().completions, clean.commander - kept_commander)
            << "the commander is asked again only what the memo did not keep";
        EXPECT_EQ(resumed_on.infantry().completions, clean.infantry - kept_infantry)
            << "the infantry is asked again only what the memo did not keep";
        EXPECT_EQ(library.article(job), clean.article) << "the article, byte for byte";
    }
    EXPECT_GT(kept_at_all, 0) << "kills that all fell before any answer was kept would test nothing";
}

// The eval's set: the lecture, in the US public domain.
constexpr std::string_view kSet = R"toml(
[[video]]
id = "weighing-radium"
title = "Weighing radium"
url = "https://www.youtube.com/watch?v=abc123XYZ_0"
type = "talk"
captions = "manual"
split = "dev"
licence = "us-public-domain"
attribution = "Science Hour"
)toml";

// The column of `name` in a CSV header.
std::size_t column(const std::string& header, std::string_view name) {
    std::size_t index = 0;
    for (std::size_t at = 0; at < header.size(); ++index) {
        const std::size_t end = std::min(header.find(',', at), header.size());
        if (std::string_view{header}.substr(at, end - at) == name) {
            return index;
        }
        at = end + 1;
    }
    return std::string::npos;
}

// The `index`th cell of a CSV row without quotes.
std::string cell(const std::string& row, std::size_t index) {
    std::size_t at = 0;
    for (std::size_t skipped = 0; skipped < index; ++skipped) {
        at = row.find(',', at) + 1;
    }
    return row.substr(at, row.find_first_of(",\r", at) - at);
}

// A panel file freezing this rreport's prompts and judges serving `sha256`.
std::string panel_for(std::string_view sha256) {
    const prompt::Library prompts = unwrap(prompt::Library::embedded());
    std::string text =
        std::format("name = \"e2e-v1\"\nprocedure = {}\ncalibrated = false\n", eval::kJudgingProcedure);
    for (const std::string_view name : eval::kJudgePrompts) {
        const prompt::Version& current = prompts.find(name)->current();
        text += std::format("[[prompt]]\nname = \"{}\"\nversion = {}\nhash = \"{}\"\n", name, current.number,
                            current.hash);
    }
    for (const std::string_view role : {"commander", "infantry"}) {
        text +=
            std::format("[[judge]]\nrole = \"{}\"\nmodel = \"{}\"\nsha256 = \"{}\"\n", role, role, sha256);
    }
    return text;
}

TEST_F(EndToEnd, AnEvalRunTakesEachReplicateThroughThenScoresAndExportsIt) {
    // The eval's directory: its set, and the lecture's snapshot, as `rreport
    // eval fetch` keeps it.
    const std::filesystem::path eval_dir = home() / "eval";
    (void)home().write_file("eval/sets/m2.toml", kSet);
    const eval::EvalSet set = unwrap(eval::read_set(eval_dir, "m2"));
    const pipeline::Source lecture{
        .origin = pipeline::Origin::web,
        .video = media::Video{.id = "abc123XYZ_0",
                              .url = "https://www.youtube.com/watch?v=abc123XYZ_0",
                              .title = "Weighing radium",
                              .channel = "Science Hour"},
        .captions = pipeline::SourceCaptions{.language = "en",
                                             .kind = media::CaptionKind::manual,
                                             .format = transcript::CaptionFormat::vtt,
                                             .content = lecture_captions()}};
    const eval::Snapshot snapshot = unwrap(eval::keep_snapshot(
        eval_dir, set.videos.front(), pipeline::to_json(lecture), std::chrono::system_clock::now()));

    const OracleRuntime runtime;
    const Library library = this->library("eval");
    const auto eval = [&](std::vector<std::string> command) {
        command.insert(command.begin(), {"eval", "--eval-dir", eval_dir.string()});
        return run(command, library, runtime);
    };
    const proc::RunResult ran = eval({"run", "--label", "base", "--replicates", "2"});
    ASSERT_EQ(ran.status.code, 0) << ran.stdout_text << ran.stderr_text;
    EXPECT_THAT(ran.stdout_text, HasSubstr("run base: 2 of 2 jobs done"));
    EXPECT_THAT(ran.stdout_text, HasSubstr(std::format("then: rreport eval score --eval-dir {} --label base",
                                                       eval_dir.string())));
    const std::int64_t asked = runtime.commander().completions + runtime.infantry().completions;

    const proc::RunResult again = eval({"run", "--label", "base", "--replicates", "2"});
    EXPECT_EQ(again.status.code, 0);
    EXPECT_THAT(again.stdout_text, HasSubstr("run base: 2 of 2 jobs done"));
    EXPECT_EQ(runtime.commander().completions + runtime.infantry().completions, asked)
        << "taken up: its jobs done, nothing asked again";
    EXPECT_EQ(eval({"run", "--label", "base", "--replicates", "3"}).status.code, 2)
        << "a label names one run";

    const proc::RunResult scored = eval({"score", "--label", "base"});
    ASSERT_EQ(scored.status.code, 0) << scored.stdout_text << scored.stderr_text;
    EXPECT_THAT(scored.stdout_text, HasSubstr("weighing-radium: no gold yet: scored without it"));
    EXPECT_THAT(scored.stdout_text, HasSubstr("run base: 2 jobs scored, by code@1"));

    const proc::RunResult exported = eval({"export", "--label", "base"});
    ASSERT_EQ(exported.status.code, 0) << exported.stderr_text;
    const std::string& csv = exported.stdout_text;
    const std::string header = csv.substr(0, csv.find('\r'));
    EXPECT_THAT(header, ::testing::StartsWith("run,tier,video,type,captions,replicate,job,scorers,"));
    const std::size_t atoms_told = column(header, "atoms_told");
    ASSERT_NE(atoms_told, std::string::npos) << header;
    const std::string first = csv.substr(csv.find('\n') + 1);
    const std::string second = first.substr(first.find('\n') + 1);
    EXPECT_THAT(first, ::testing::StartsWith("base,max,weighing-radium,talk,manual,0,"));
    EXPECT_THAT(second, ::testing::StartsWith("base,max,weighing-radium,talk,manual,1,"));
    EXPECT_EQ(cell(first, atoms_told), "1");
    EXPECT_EQ(cell(second, atoms_told), "1");

    const std::filesystem::path exported_json = home() / "base.json";
    ASSERT_EQ(
        eval({"export", "--label", "base", "--format", "json", "--out", exported_json.string()}).status.code,
        0);
    const json::Json read = unwrap(json::parse(unwrap(read_file(exported_json))));
    ASSERT_EQ(read.at("entries").size(), 2U);
    EXPECT_EQ(read.at("entries")[1].at("scores").at("code@1").at("sentences_accounted"), 1.0) << "by scorer";

    // A person's gold for the video, reviewed for the snapshot kept; and a
    // panel frozen for this rreport's prompts and the files this runtime
    // serves, both engines the same pretend weights.
    const eval::Gold gold{.video = "weighing-radium",
                          .snapshot = snapshot.hash,
                          .status = eval::GoldStatus::gold,
                          .reviewer = "Ada",
                          .facts = {eval::Fact{.id = "F1",
                                               .start = 90s,
                                               .end = 115s,
                                               .importance = eval::Importance::core,
                                               .quote = "The NRC lent us 3 scales",
                                               .claim = "The NRC lent them three scales."}}};
    (void)home().write_file("eval/gold/weighing-radium.toml", eval::to_toml(gold));
    (void)home().write_file("eval/judges/e2e-v1.toml", panel_for(sha256(component::kModelWeights).hex()));
    const std::int64_t before = runtime.commander().completions + runtime.infantry().completions;

    const proc::RunResult judged = eval({"judge", "--label", "base", "--panel", "e2e-v1"});
    ASSERT_EQ(judged.status.code, 0) << judged.stdout_text << judged.stderr_text;
    EXPECT_THAT(judged.stdout_text,
                HasSubstr("run base: 2 of 2 jobs judged, by judges@e2e-v1 (reported only: no person has "
                          "calibrated the panel yet)"));
    const std::int64_t judging = runtime.commander().completions + runtime.infantry().completions;
    EXPECT_GT(judging, before) << "both judges asked";
    EXPECT_EQ(eval({"judge", "--label", "base", "--panel", "e2e-v1"}).status.code, 0);
    EXPECT_EQ(runtime.commander().completions + runtime.infantry().completions, judging)
        << "its judgements done: nothing asked again";

    const std::string both = eval({"export", "--label", "base"}).stdout_text;
    const std::string both_header = both.substr(0, both.find('\r'));
    const std::string both_first = both.substr(both.find('\n') + 1);
    EXPECT_THAT(both_first, HasSubstr(",code@1+judges@e2e-v1,"))
        << "the code's scores and the judges', side by side";
    EXPECT_EQ(cell(both_first, column(both_header, "facts_told")), "1");
    EXPECT_EQ(cell(both_first, column(both_header, "sentences_supported")), "1");
    EXPECT_EQ(cell(both_first, column(both_header, "atoms_told")), "1") << "the code's, kept";
}

// The lecture under two names: two videos, as an interval needs.
constexpr std::string_view kTwoVideos = R"toml(
[[video]]
id = "weighing-radium"
title = "Weighing radium"
url = "https://www.youtube.com/watch?v=abc123XYZ_0"
type = "talk"
captions = "manual"
split = "dev"
licence = "us-public-domain"
attribution = "Science Hour"

[[video]]
id = "radium-again"
title = "Weighing radium, again"
url = "https://www.youtube.com/watch?v=abc123XYZ_1"
type = "talk"
captions = "manual"
split = "dev"
licence = "us-public-domain"
attribution = "Science Hour"
)toml";

constexpr std::string_view kGate = R"toml(name = "e2e"
[[invariant]]
metric = "sentences_accounted"
every_entry = 1
[[invariant]]
metric = "invalid_markers"
every_entry = 0
[[gated]]
metric = "atoms_told"
margin = 0.01
[[gated]]
metric = "facts_told"
margin = 0.02
)toml";

// The set of the lecture under two names in `eval_dir`, each video's
// snapshot kept as `eval fetch` keeps it.
void keep_two_videos(const std::filesystem::path& eval_dir) {
    const std::filesystem::path sets = eval_dir / "sets";
    std::filesystem::create_directories(sets);
    unwrap(write_file_atomically(sets / "m2.toml", kTwoVideos));
    for (const eval::EvalVideo& video : unwrap(eval::read_set(eval_dir, "m2")).videos) {
        const pipeline::Source source{
            .origin = pipeline::Origin::web,
            .video = media::Video{.id = video.url.substr(video.url.find('=') + 1),
                                  .url = video.url,
                                  .title = video.title,
                                  .channel = "Science Hour"},
            .captions = pipeline::SourceCaptions{.language = "en",
                                                 .kind = media::CaptionKind::manual,
                                                 .format = transcript::CaptionFormat::vtt,
                                                 .content = lecture_captions()}};
        (void)unwrap(eval::keep_snapshot(eval_dir, video, pipeline::to_json(source),
                                         std::chrono::system_clock::now()));
    }
}

TEST_F(EndToEnd, TwoRunsOfOnePipelineAreComparedAndGatedAndAnAaRunHoldsTheGatesMargins) {
    const std::filesystem::path eval_dir = home() / "eval";
    keep_two_videos(eval_dir);
    (void)home().write_file("eval/gates/e2e.toml", kGate);
    (void)home().write_file("eval/judges/e2e-v1.toml", panel_for(sha256(component::kModelWeights).hex()));
    const OracleRuntime runtime;
    const Library library = this->library("eval");
    const auto eval = [&](std::vector<std::string> command) {
        command.insert(command.begin(), {"eval", "--eval-dir", eval_dir.string()});
        return run(command, library, runtime);
    };
    const auto asked = [&] { return runtime.commander().completions + runtime.infantry().completions; };

    ASSERT_EQ(eval({"run", "--label", "base", "--replicates", "2"}).status.code, 0);
    ASSERT_EQ(eval({"score", "--label", "base"}).status.code, 0);
    const std::int64_t base = asked();
    ASSERT_EQ(eval({"run", "--label", "cand", "--replicates", "2"}).status.code, 0);
    EXPECT_EQ(asked(), base) << "the same pipeline under the same salts: every stage run shared";
    ASSERT_EQ(eval({"score", "--label", "cand"}).status.code, 0);

    const proc::RunResult compared = eval({"compare", "base", "cand", "--gate", "e2e"});
    ASSERT_EQ(compared.status.code, 0) << compared.stderr_text;
    EXPECT_THAT(compared.stdout_text, HasSubstr("A: base (m2 dev, tier max, replicates 0 to 1)\n"));
    EXPECT_THAT(compared.stdout_text,
                HasSubstr("  atoms_told                    2     1.000     1.000    +0.000   +0.000 "
                          "to +0.000       0.000   0.000   0.010   no difference\n"));

    // Flow: each video's two articles read side by side, both orders, by
    // both judges of a panel frozen for the files the runtime serves.
    const std::int64_t unread = asked();
    const proc::RunResult flowed =
        eval({"compare", "base", "cand", "--gate", "e2e", "--flow", "--panel", "e2e-v1"});
    ASSERT_EQ(flowed.status.code, 0) << flowed.stdout_text << flowed.stderr_text;
    EXPECT_THAT(flowed.stdout_text,
                HasSubstr("  flow                          2         -         -    +0.000   +0.000 to +0.000"
                          "       0.000   0.000       -   no difference\n"))
        << "one pipeline's articles, alike: every part a tie";
    EXPECT_GT(asked(), unread) << "the judges read each part";
    const std::int64_t read = asked();
    const proc::RunResult by_video =
        eval({"compare", "base", "cand", "--gate", "e2e", "--flow", "--panel", "e2e-v1", "--metric", "flow"});
    ASSERT_EQ(by_video.status.code, 0) << by_video.stderr_text;
    EXPECT_THAT(by_video.stdout_text, HasSubstr("  radium-again                    +0.000\n"));
    EXPECT_THAT(by_video.stdout_text, HasSubstr("  weighing-radium                 +0.000\n"));
    EXPECT_EQ(asked(), read) << "pairs read once, never asked again";

    const proc::RunResult gated = eval({"gate", "base", "cand", "--gate", "e2e", "--panel", "e2e-v1"});
    EXPECT_EQ(gated.status.code, 0) << gated.stdout_text << gated.stderr_text;
    EXPECT_THAT(gated.stdout_text,
                HasSubstr("sentences_accounted    at least 1.000   passed: kept by 4 entries\n"));
    EXPECT_THAT(gated.stdout_text,
                HasSubstr("facts_told             margin 0.020     ignored: the panel e2e-v1 judges it, "
                          "and no person has calibrated the panel yet: not counted\n"));
    EXPECT_THAT(gated.stdout_text, HasSubstr("\ngate e2e: passed\n"));

    ASSERT_EQ(eval({"run", "--label", "base-aa", "--replicates", "2", "--first-replicate", "2"}).status.code,
              0);
    EXPECT_GT(asked(), base) << "other salts: every sampled stage drawn again";
    ASSERT_EQ(eval({"score", "--label", "base-aa"}).status.code, 0);
    const proc::RunResult aa =
        eval({"gate", "base", "base-aa", "--aa", "--gate", "e2e", "--panel", "e2e-v1"});
    EXPECT_EQ(aa.status.code, 0) << aa.stdout_text << aa.stderr_text;
    EXPECT_THAT(aa.stdout_text,
                HasSubstr("an unchanged pipeline passes every counted gated metric at once 100% of "
                          "the time (80% at least, ADR-0019)\n"))
        << "the oracle draws alike under every salt: no noise at all";
    EXPECT_THAT(aa.stdout_text, HasSubstr("gate e2e: its margins hold\n"));
    const proc::RunResult shared =
        eval({"gate", "base", "cand", "--aa", "--gate", "e2e", "--panel", "e2e-v1"});
    EXPECT_EQ(shared.status.code, 2) << "cand draws under base's salts: no A/A run";

    // Labels (P27): a reviewed gold of one fact for one video, the baseline
    // judged; then what the judges read of it drawn for a person, with the
    // parts its articles were read against cand's.
    const eval::EvalVideo weighing = unwrap(eval::read_set(eval_dir, "m2")).videos.front();
    ASSERT_EQ(weighing.id, "weighing-radium");
    const eval::Gold gold{.video = weighing.id,
                          .snapshot = unwrap(eval::read_snapshot(eval_dir, weighing))->hash,
                          .status = eval::GoldStatus::gold,
                          .reviewer = "Ada",
                          .facts = {eval::Fact{.id = "F1",
                                               .start = 90s,
                                               .end = 115s,
                                               .importance = eval::Importance::core,
                                               .quote = "The NRC lent us 3 scales",
                                               .claim = "The NRC lent them three scales."}}};
    (void)home().write_file("eval/gold/weighing-radium.toml", eval::to_toml(gold));
    ASSERT_EQ(eval({"judge", "--label", "base", "--panel", "e2e-v1"}).status.code, 0);
    const proc::RunResult labelled =
        eval({"label", "r5", "--label", "base", "--against", "cand", "--panel", "e2e-v1"});
    ASSERT_EQ(labelled.status.code, 0) << labelled.stdout_text << labelled.stderr_text;
    EXPECT_THAT(labelled.stdout_text,
                HasSubstr("give each item its verdict, set labeller to your name, then: "
                          "rreport eval calibrate r5\n"));
    eval::Labels labels = unwrap(eval::read_labels(eval_dir, "r5"));
    EXPECT_EQ(labels.facts.size(), 2U) << "F1, in each of its video's two replicates";
    EXPECT_EQ(labels.sentences.size(), 15U);
    EXPECT_EQ(labels.parts.size(), 8U)
        << "every part read, fewer than ten: two videos, two pairs, two parts each";
    EXPECT_THAT(labelled.stdout_text, HasSubstr("and 8 of the 8 parts the panel read\n"));

    // A person who reads as the oracle does: the fact told, every sentence
    // supported, every part alike in both runs. One class a kind: κ cannot
    // tell such a person from a coin, and the panel does not pass.
    for (eval::FactLabel& label : labels.facts) {
        label.verdict = eval::FactVerdict::told;
    }
    for (eval::SentenceLabel& label : labels.sentences) {
        label.verdict = pipeline::Issue::none;
    }
    for (eval::PartLabel& label : labels.parts) {
        label.verdict = reason::Preference::tie;
    }
    EXPECT_EQ(eval({"calibrate", "r5"}).status.code, 2) << "no labeller yet";
    labels.labeller = "Ada";
    unwrap(write_file_atomically(eval::labels_file(eval_dir, "r5"), eval::to_toml(labels)));
    const proc::RunResult calibrated = eval({"calibrate", "r5"});
    EXPECT_EQ(calibrated.status.code, 1) << calibrated.stdout_text << calibrated.stderr_text;
    EXPECT_THAT(calibrated.stdout_text,
                HasSubstr("labels r5, by Ada: run base against cand, panel e2e-v1\n"));
    EXPECT_THAT(calibrated.stdout_text, HasSubstr("\nfacts: 2 labelled, 0 skipped\n"));
    EXPECT_THAT(calibrated.stdout_text, HasSubstr("facts, commander: one class only"));
    EXPECT_THAT(calibrated.stdout_text, HasSubstr("the panel e2e-v1 does not pass yet:\n"));
    EXPECT_EQ(eval({"label", "r5", "--label", "base", "--against", "cand", "--panel", "e2e-v1"}).status.code,
              2)
        << "a person's verdicts are never written over";
}

// A tier that reads each window once, and again where the infantry was
// unsure (P28). The oracle's engines send every token's probability, as
// llama-server sends them, through the runtime; each is certain, so each
// window is read once. A probability lost on its way is a sentence not
// known to be sure, and its window would be read again.
TEST_F(EndToEnd, AnInfantrySureOfEveryReadingReadsEachWindowOnce) {
    const OracleRuntime runtime;
    const Library library = this->library("sure");
    const proc::RunResult ingested =
        run({"ingest", captions().string(), "--tier", "max+atomize.first=1+atomize.step=1+atomize.sure=0.9"},
            library, runtime);
    ASSERT_EQ(ingested.status.code, 0) << ingested.stderr_text;
    const std::map<std::string, std::int64_t> missions = library.missions(job_in(ingested.stderr_text));
    std::size_t windows = 0;
    for (const auto& [mission, samples] : missions) {
        EXPECT_THAT(mission, Not(HasSubstr("-wave-"))) << "no window read again";
        if (mission.starts_with("atomize-")) {
            ++windows;
            EXPECT_EQ(samples, 1) << mission << ": one sample, as the tier's first wave asks";
        }
    }
    EXPECT_GT(windows, 0U);
    EXPECT_THAT(ingested.stderr_text, Not(HasSubstr("wave 2")));
}

// A variant that fuses each section's drafts into one more (P29), through
// the real executables: the fake engine's oracle answers the fusions too.
TEST_F(EndToEnd, AFusingVariantWritesOneDraftMoreOfEachSection) {
    const OracleRuntime runtime;
    const Library library = this->library("fuse");
    const proc::RunResult ingested =
        run({"ingest", captions().string(), "--tier", "max+write.fuse=true"}, library, runtime);
    ASSERT_EQ(ingested.status.code, 0) << ingested.stderr_text;
    const std::map<std::string, std::int64_t> missions = library.missions(job_in(ingested.stderr_text));
    std::size_t written = 0;
    std::size_t fused = 0;
    for (const auto& [mission, samples] : missions) {
        if (mission.starts_with("write-")) {
            ++written;
        } else if (mission.starts_with("fuse-")) {
            ++fused;
            EXPECT_EQ(samples, 1) << mission << ": one draft more";
        }
    }
    EXPECT_GT(written, 0U);
    EXPECT_EQ(fused, written) << "every writing mission's drafts fused";
    EXPECT_THAT(ingested.stderr_text, HasSubstr("write: the commander fuses the drafts of"));
    EXPECT_THAT(ingested.stderr_text, HasSubstr("fused drafts kept")) << "the fusions answered, and passed";
}

// A sweep of one variant: one draft instead of four. The variant shares
// every stage before the writer with the baseline, under its salts.
constexpr std::string_view kSweep = R"toml(name = "s"
baseline = "max"
replicates = 1

[[variant]]
label = "one-draft"
tier = "max+write.drafts=1+write.judge_top=0"
why = "Whether four drafts are worth their GPU time."
)toml";

TEST_F(EndToEnd, ASweepRunsEachVariantUnderTheBaselinesSaltsAndThePageShowsEveryRun) {
    const std::filesystem::path eval_dir = home() / "eval";
    keep_two_videos(eval_dir);
    (void)home().write_file("eval/sweeps/s.toml", kSweep);
    const OracleRuntime runtime;
    const Library library = this->library("eval");
    const auto eval = [&](std::vector<std::string> command) {
        command.insert(command.begin(), {"eval", "--eval-dir", eval_dir.string()});
        return run(command, library, runtime);
    };
    const auto asked = [&] { return runtime.commander().completions + runtime.infantry().completions; };

    ASSERT_EQ(eval({"run", "--label", "s-baseline", "--replicates", "1"}).status.code, 0)
        << "the baseline run first, as a person may: the sweep takes it up";
    const std::int64_t baseline = asked();
    const proc::RunResult swept = eval({"sweep", "s", "--no-judge"});
    ASSERT_EQ(swept.status.code, 0) << swept.stdout_text << swept.stderr_text;
    const std::int64_t variant = asked() - baseline;
    EXPECT_GT(variant, 0) << "its writer drafts again";
    EXPECT_LT(variant, baseline) << "every stage before the writer the baseline's own";
    EXPECT_THAT(swept.stdout_text,
                HasSubstr("sweep s: each variant against the baseline, max (s-baseline), on the dev split, 1 "
                          "replicate\n"));
    EXPECT_THAT(swept.stdout_text,
                HasSubstr("\none-draft: max+write.drafts=1+write.judge_top=0\n  Whether four "
                          "drafts are worth their GPU time.\n"));
    EXPECT_THAT(swept.stdout_text,
                HasSubstr("  every metric: rreport eval compare s-baseline s-one-draft\n"));
    const std::int64_t done = asked();
    ASSERT_EQ(eval({"sweep", "s", "--no-judge"}).status.code, 0);
    EXPECT_EQ(asked(), done) << "a sweep done, asked again, asks nothing";

    const std::filesystem::path page = home() / "report.html";
    const proc::RunResult reported = eval({"report", "--out", page.string()});
    ASSERT_EQ(reported.status.code, 0) << reported.stderr_text;
    EXPECT_THAT(reported.stdout_text, HasSubstr("2 runs, atoms_told against minutes_per_video_hour"));
    const std::string html = unwrap(read_file(page));
    EXPECT_THAT(html, HasSubstr("<h2>atoms_told against minutes_per_video_hour</h2>"));
    std::size_t plotted = 0;
    for (std::size_t at = html.find("<circle "); at != std::string::npos;
         at = html.find("<circle ", at + 1)) {
        ++plotted;
    }
    EXPECT_EQ(plotted, 2U) << "both runs, each at its quality and its cost";
    EXPECT_THAT(html, HasSubstr("<td>s-one-draft</td>"));
}

}  // namespace
}  // namespace rr
