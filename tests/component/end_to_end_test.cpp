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
#include <rr/core/result.hpp>
#include <rr/db/database.hpp>
#include <rr/eval/set.hpp>
#include <rr/eval/snapshot.hpp>
#include <rr/json/json.hpp>
#include <rr/library/library.hpp>
#include <rr/library/stage_store.hpp>
#include <rr/net/http_client.hpp>
#include <rr/proc/process.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

#include "../unit/pipeline/lecture.hpp"
#include "runtime_process.hpp"

namespace rr {
namespace {

using namespace std::chrono_literals;
using ::testing::HasSubstr;
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
    (void)unwrap(eval::keep_snapshot(eval_dir, set.videos.front(), pipeline::to_json(lecture),
                                     std::chrono::system_clock::now()));

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
    EXPECT_THAT(header, ::testing::StartsWith("run,tier,video,type,captions,replicate,job,scorer,"));
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
    EXPECT_EQ(read.at("entries")[1].at("scores").at("sentences_accounted"), 1.0);
}

}  // namespace
}  // namespace rr
