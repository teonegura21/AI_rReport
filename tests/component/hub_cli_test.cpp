// The rreport executable, driven from the outside against a real
// rreport-runtime (with fake engines: a commander, and infantry with four
// slots) and a fake RunPod API: what an operator runs before the first video.
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/core/clock.hpp>
#include <rr/core/file_io.hpp>
#include <rr/core/hash.hpp>
#include <rr/eval/panel.hpp>
#include <rr/eval/run.hpp>
#include <rr/fakes/fake_runpod.hpp>
#include <rr/json/json.hpp>
#include <rr/library/call_log.hpp>
#include <rr/library/jobs.hpp>
#include <rr/library/library.hpp>
#include <rr/proc/process.hpp>
#include <rr/prompt/library.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

#include "runtime_process.hpp"

namespace rr {
namespace {

using namespace std::chrono_literals;
using fakes::FakeRunpod;
using ::testing::HasSubstr;
using ::testing::Not;
using testkit::unwrap;
using Clock = std::chrono::steady_clock;

// rreport-runtime's catalog: a commander ("chat", two slots sharing one
// pool, for answers drawn two at a time, and a prompt cache) and infantry
// ("squad", four slots with a share each, and no prompt cache), both fake.
constexpr std::string_view kCatalog = R"toml(
[[model]]
id = "chat"
role = "commander"
file = "chat.gguf"
sha256 = "{sha}"
[[model]]
id = "squad"
role = "infantry"
file = "squad.gguf"
sha256 = "{sha}"

[[profile]]
name = "test"
  [[profile.engine]]
  model = "chat"
  slots = 2
  context = 4096
  kv_unified = true
  extra_args = ["--fake-token-ms", "2", "--fake-prompt-cache"]
  [[profile.engine]]
  model = "squad"
  slots = 4
  extra_args = ["--fake-token-ms", "2"]
)toml";

// The hub's side: a data directory, a pod at RunPod (faked), and the runtime.
class Hub : public ::testing::Test {
protected:
    Hub() { runpod_.add_pod("pod123", "RUNNING"); }

    [[nodiscard]] FakeRunpod& runpod() { return runpod_; }
    [[nodiscard]] const testkit::TempDir& home() const { return home_; }
    [[nodiscard]] std::filesystem::path data_dir() const { return home_ / "data"; }

    // Runs `rreport <command…>` with settings aimed at the fakes, then `extra`
    // settings (the last value of a setting wins).
    [[nodiscard]] proc::RunResult rreport(std::vector<std::string> command,
                                          const std::vector<std::string>& extra = {},
                                          std::vector<EnvironmentVariable> environment = {}) const {
        std::vector<std::string> argv{RR_HUB_BINARY};
        argv.insert(argv.end(), command.begin(), command.end());
        std::vector<std::string> settings{
            std::format("hub.data_dir={}", data_dir().string()),
            std::format("runtime.url={}", runtime_.url()), "runpod.pod_id=pod123",
            std::format("runpod.api_url={}", runpod_.api_url()), "log.level=warn"};
        settings.insert(settings.end(), extra.begin(), extra.end());
        for (const std::string& setting : settings) {
            argv.emplace_back("--set");
            argv.push_back(setting);
        }
        environment.emplace_back("HOME", home_.path().string());
        environment.emplace_back("RREPORT_RUNTIME__TOKEN", std::string{component::kRuntimeToken});
        environment.emplace_back("RREPORT_RUNPOD__API_KEY", std::string{FakeRunpod::kAccountKey});
        environment.emplace_back("PATH", "/usr/bin:/bin");
        return unwrap(proc::run(
            proc::Command{
                .argv = std::move(argv), .environment = std::move(environment), .inherit_environment = false},
            proc::RunOptions{.timeout = 120s}));
    }

private:
    testkit::TempDir home_{"rr_hub_cli_home"};
    FakeRunpod runpod_;
    component::RuntimeProcess runtime_{kCatalog, {"chat.gguf", "squad.gguf"}};
};

TEST_F(Hub, DoctorChecksEverythingAndSaysSo) {
    // The engines load after the runtime starts listening: wait, as an operator would.
    ASSERT_EQ(rreport({"pod", "start"}).status.code, 0);
    const proc::RunResult result = rreport({"doctor"});
    EXPECT_EQ(result.status.code, 0) << result.stdout_text << result.stderr_text;
    // ok, or a warning on a disk with little room (CI runners): either way, found and writable.
    EXPECT_THAT(result.stdout_text, HasSubstr(std::format("] data directory: '{}'", data_dir().string())));
    EXPECT_THAT(result.stdout_text, HasSubstr(std::format("[ok  ] library: 'default' (schema version {})",
                                                          library::migrations().back().version)));
    EXPECT_THAT(result.stdout_text, HasSubstr("[ok  ] RunPod: pod pod123 is RUNNING"));
    EXPECT_THAT(result.stdout_text, HasSubstr("[ok  ] runtime:"));
    EXPECT_THAT(result.stdout_text, HasSubstr("chat (commander) ready, squad (infantry) ready"));
    EXPECT_THAT(result.stdout_text, Not(HasSubstr("[warn] versions"))) << "one build, one version";
}

TEST_F(Hub, DoctorExplainsWhatIsWrong) {
    const proc::RunResult wrong_token = rreport({"doctor"}, {"runtime.token=not-the-token"});
    EXPECT_EQ(wrong_token.status.code, 1) << "a failure";
    EXPECT_THAT(wrong_token.stdout_text, HasSubstr("[FAIL] runtime"));
    EXPECT_THAT(wrong_token.stdout_text, HasSubstr("must equal the pod's RR_RUNTIME_TOKEN"));

    runpod().add_pod("pod123", "EXITED");
    const proc::RunResult stopped = rreport({"doctor"});
    EXPECT_EQ(stopped.status.code, 0) << "a stopped pod is how it should be when idle";
    EXPECT_THAT(stopped.stdout_text, HasSubstr("[warn] runtime: not checked: the pod is EXITED"));
    EXPECT_THAT(stopped.stdout_text, HasSubstr("rreport pod start"));

    const proc::RunResult bad_key = rreport({"doctor"}, {"runpod.api_key=wrong"});
    EXPECT_EQ(bad_key.status.code, 1);
    EXPECT_THAT(bad_key.stdout_text, HasSubstr("RunPod refused the API key"));
}

TEST_F(Hub, StartsStopsAndReportsThePod) {
    runpod().add_pod("pod123", "EXITED");
    const proc::RunResult started = rreport({"pod", "start"});
    EXPECT_EQ(started.status.code, 0) << started.stderr_text;
    EXPECT_THAT(started.stdout_text, HasSubstr("pod pod123 is RUNNING"));
    EXPECT_THAT(started.stdout_text, HasSubstr("every engine ready"));
    EXPECT_EQ(runpod().actions("start"), 1);

    const proc::RunResult status = rreport({"pod", "status"});
    EXPECT_EQ(status.status.code, 0);
    EXPECT_THAT(status.stdout_text, HasSubstr("$0.44/h"));
    EXPECT_THAT(status.stdout_text, HasSubstr("profile test, no GPU")) << "a test machine has none";
    EXPECT_THAT(status.stdout_text,
                HasSubstr("squad (infantry): ready, 4 slot(s), up to 2048 tokens each\n"));
    EXPECT_THAT(status.stdout_text,
                HasSubstr("chat (commander): ready, 2 slot(s), up to 4096 tokens each from a shared pool"));

    const proc::RunResult stopped = rreport({"pod", "stop"});
    EXPECT_EQ(stopped.status.code, 0);
    EXPECT_THAT(stopped.stdout_text, HasSubstr("pod pod123 is EXITED"));
    EXPECT_EQ(runpod().status("pod123"), "EXITED");
}

TEST_F(Hub, BenchMeasuresThePodAndKeepsTheResults) {
    const std::filesystem::path out = home() / "bench.json";
    const proc::RunResult result = rreport({"bench", "--parallel", "1,2,6", "--output-tokens", "8",
                                            "--rounds", "2", "--prompt-tokens", "64", "--out", out.string()});
    ASSERT_EQ(result.status.code, 0) << result.stdout_text << result.stderr_text;
    EXPECT_THAT(result.stdout_text, HasSubstr("## Commander `chat`"));
    EXPECT_THAT(result.stdout_text, HasSubstr("## Infantry `squad`"));
    EXPECT_THAT(result.stdout_text, HasSubstr("| 6 (queued) | 4 |")) << "more requests than slots";
    EXPECT_THAT(result.stdout_text, HasSubstr("Price: $0.44 per hour")) << "RunPod's price for the pod";

    const json::Json saved = unwrap(json::parse(unwrap(read_file(out))));
    EXPECT_EQ(saved["commander"]["model"], "chat");
    EXPECT_EQ(saved["commander"]["rounds"], 2);
    ASSERT_EQ(saved["infantry"]["rows"].size(), 3U);
    EXPECT_EQ(saved["infantry"]["rows"][1]["tokens"], 16) << "two answers of exactly eight tokens";
    EXPECT_EQ(saved["infantry"]["rows"][2]["failed"], 0);
    EXPECT_GT(saved["commander"]["generation_tokens_per_second"].get<double>(), 0);

    // Every call is accounted for in the library: 2 + 1 + 2 + 6.
    db::Database library =
        unwrap(library::open_database(data_dir() / "libraries" / "default" / "library.db"));
    const library::CallLog calls{library};
    EXPECT_EQ(unwrap(calls.list({.purpose = "bench"})).size(), 11U);
}

TEST_F(Hub, BenchAsksTheCommanderForSeveralAnswersAtOnce) {
    const std::filesystem::path out = home() / "drafts.json";
    const proc::RunResult drafts = rreport({"bench", "--n", "2", "--parallel", "1", "--output-tokens", "8",
                                            "--rounds", "1", "--prompt-tokens", "32", "--out", out.string()});
    ASSERT_EQ(drafts.status.code, 0) << drafts.stdout_text << drafts.stderr_text;
    EXPECT_THAT(drafts.stdout_text, HasSubstr("## Commander `chat`, 2 answers per request"));
    EXPECT_EQ(unwrap(json::parse(unwrap(read_file(out))))["commander"]["choices"], 2);

    db::Database library =
        unwrap(library::open_database(data_dir() / "libraries" / "default" / "library.db"));
    const std::vector<llm::CallRecord> calls = unwrap(library::CallLog{library}.list({.purpose = "bench"}));
    const auto commander =
        std::ranges::find_if(calls, [](const llm::CallRecord& call) { return call.model == "chat"; });
    ASSERT_NE(commander, calls.end());
    EXPECT_EQ(commander->usage.completion_tokens, 16) << "two answers of exactly eight tokens, one call";

    // A short call timeout: without the check, llama-server would hold this request.
    const proc::RunResult too_many =
        rreport({"bench", "--n", "3", "--parallel", "1", "--rounds", "1"}, {"llm.call_timeout=20s"});
    EXPECT_EQ(too_many.status.code, 1);
    EXPECT_THAT(too_many.stderr_text, HasSubstr("3 answers per request need 3 commander slots at once"))
        << "llama-server would hold such a request forever";
}

TEST_F(Hub, BenchWeighsTheCommandersSlotsAndThePromptCache) {
    const std::filesystem::path out = home() / "slots.json";
    const proc::RunResult result =
        rreport({"bench", "--commander-parallel", "1,2,3", "--cache-check", "--parallel", "1", "--rounds",
                 "1", "--output-tokens", "8", "--prompt-tokens", "200", "--out", out.string()});
    ASSERT_EQ(result.status.code, 0) << result.stdout_text << result.stderr_text;

    // The engines, as the pipeline will size its work by them.
    EXPECT_THAT(result.stdout_text, HasSubstr("| `chat` | commander | 2 | 4096 | 4096 | shared |"));
    EXPECT_THAT(result.stdout_text, HasSubstr("| `squad` | infantry | 4 | 8192 | 2048 | split |"));
    // The commander at several requests at once, the third waiting for a slot.
    EXPECT_THAT(result.stdout_text, HasSubstr("Several requests at once:"));
    EXPECT_THAT(result.stdout_text, HasSubstr("| 3 (queued) | 2 |"));
    // One engine's cache serves the shared system message; the other's cannot.
    EXPECT_THAT(result.stdout_text, HasSubstr("## Prompt cache"));
    EXPECT_THAT(result.stdout_text, HasSubstr("- `chat`: reused, also by 2 requests at once"));
    EXPECT_THAT(result.stdout_text,
                HasSubstr("- `squad`: NOT reused: each request reads the whole system message again"));

    json::Json saved = unwrap(json::parse(unwrap(read_file(out))));
    EXPECT_EQ(saved["engines"][0]["ctx_per_slot"], 4096);
    EXPECT_EQ(saved["engines"][0]["kv_unified"], true);
    EXPECT_EQ(saved["engines"][0]["model_sha256"], sha256(component::kModelWeights).hex())
        << "which file was measured";
    ASSERT_EQ(saved["commander"]["parallel"].size(), 3U);
    EXPECT_EQ(saved["commander"]["parallel"][1]["tokens"], 16) << "two answers of exactly eight tokens";
    ASSERT_EQ(saved["cache"].size(), 2U);
    json::Json chat = saved["cache"][0];
    EXPECT_EQ(chat["model"], "chat");
    EXPECT_EQ(chat["reused"], true);
    EXPECT_EQ(chat["first"]["cached_tokens"], 0) << "the run's own system message: nothing to reuse";
    EXPECT_GT(chat["next"]["cached_tokens"].get<int>(), chat["next"]["prompt_tokens"].get<int>() * 9 / 10)
        << "all but the question";
    EXPECT_EQ(chat["together"]["at_once"], 2) << "as many as the engine's slots, at most four";
    json::Json squad = saved["cache"][1];
    EXPECT_EQ(squad["reused"], false);
    EXPECT_EQ(squad["together"]["at_once"], 4);
    EXPECT_EQ(squad["next"]["cached_tokens"], 0);

    // Every call is accounted for: 1 + (1 + 2 + 3) for the commander, 1 for
    // the infantry, and (1 + 1 + 2) + (1 + 1 + 4) to check the caches.
    db::Database library =
        unwrap(library::open_database(data_dir() / "libraries" / "default" / "library.db"));
    EXPECT_EQ(unwrap(library::CallLog{library}.list({.purpose = "bench"})).size(), 18U);
}

TEST_F(Hub, BenchKeepsALevelTheEngineCannotServeAsAResult) {
    // 3000 tokens: the commander's shared pool of 4096 holds one such
    // request; the infantry's share of 2048 per slot holds none.
    const std::filesystem::path out = home() / "long.json";
    const proc::RunResult result = rreport({"bench", "--prompt-tokens", "3000", "--output-tokens", "4",
                                            "--rounds", "1", "--parallel", "1,2", "--out", out.string()});
    ASSERT_EQ(result.status.code, 0) << "a limit found is a result, not a failure of the bench\n"
                                     << result.stdout_text << result.stderr_text;
    EXPECT_THAT(result.stdout_text, HasSubstr("## Commander `chat`"));
    EXPECT_THAT(result.stdout_text, HasSubstr("| 2 | 4 | \u2013 | \u2013 | \u2013 | 2 | \u2013 |"));
    EXPECT_THAT(
        result.stdout_text,
        HasSubstr("- 2 at once: 2 of 2 failed: HTTP 400: the request exceeds the available context size"));
    const json::Json saved = unwrap(json::parse(unwrap(read_file(out))));
    EXPECT_EQ(saved["infantry"]["rows"][0]["failed"], 1);
    EXPECT_THAT(saved["infantry"]["rows"][0]["error"].get<std::string>(),
                HasSubstr("exceeds the available context size"));
    EXPECT_GT(saved["commander"]["generation_tokens_per_second"].get<double>(), 0) << "the commander served";
}

TEST_F(Hub, BenchTimesAColdStart) {
    const std::filesystem::path out = home() / "cold.json";
    const proc::RunResult result =
        rreport({"bench", "--cold-start", "--parallel", "1", "--output-tokens", "4", "--rounds", "1",
                 "--prompt-tokens", "32", "--out", out.string()});
    ASSERT_EQ(result.status.code, 0) << result.stdout_text << result.stderr_text;
    EXPECT_THAT(result.stdout_text, HasSubstr("Cold start:"));
    EXPECT_EQ(runpod().actions("stop"), 1);
    EXPECT_EQ(runpod().actions("start"), 1);
    EXPECT_TRUE(unwrap(json::parse(unwrap(read_file(out))))["cold_start_ms"].is_number());
}

// --- Jobs ---------------------------------------------------------------------------------

constexpr std::string_view kCaptions =
    "WEBVTT\n\n00:00.000 --> 00:03.000\nRadium glows in the dark.\n\n00:03.000 --> 00:06.000\n"
    "Pierre Curie heated it until it glowed.\n";

// The job id a run of `rreport ingest` or `resume` names: "job <id> …".
std::string job_in(const std::string& output) {
    const std::size_t at = output.rfind("job ");
    return at == std::string::npos ? std::string{} : output.substr(at + 4, 26);
}

// Without a pod, the stages that can do without models do, and say what
// they could not do; the atomize stage cannot, and the job waits for a pod.
// With one, every stage runs (end_to_end_test.cpp).
TEST_F(Hub, IngestsACaptionFileWithoutAPodUntilItNeedsOne) {
    const std::vector<std::string> no_pod{"runtime.url=", "runpod.pod_id="};
    const std::filesystem::path captions = home().write_file("On radium.vtt", kCaptions);
    const proc::RunResult first = rreport({"ingest", captions.string()}, no_pod);
    EXPECT_EQ(first.status.code, 1) << first.stdout_text << first.stderr_text;
    EXPECT_THAT(first.stderr_text, HasSubstr("transcript: done"));
    EXPECT_THAT(first.stderr_text, HasSubstr("outline: done"));
    EXPECT_THAT(first.stdout_text, HasSubstr("what the stages could not do:"));
    EXPECT_THAT(first.stdout_text,
                HasSubstr("outline: No pod is configured to read the video: its sections are cut "
                          "where the speaker paused"));
    const std::string job = job_in(first.stdout_text);
    EXPECT_THAT(first.stdout_text,
                HasSubstr(std::format("job {} needs you: atomizing needs models, and no pod is configured: "
                                      "rreport doctor says how to set one up\n  then: rreport resume {}",
                                      job, job)));

    const proc::RunResult second = rreport({"ingest", captions.string()}, no_pod);
    EXPECT_EQ(second.status.code, 1);
    EXPECT_THAT(second.stderr_text, HasSubstr("source: found done"));
    EXPECT_THAT(second.stderr_text, HasSubstr("transcript: found done")) << "the stage cache answers";
    EXPECT_THAT(second.stderr_text, HasSubstr("outline: found done"));

    const proc::RunResult listed = rreport({"jobs"});
    ASSERT_EQ(listed.status.code, 0);
    EXPECT_THAT(listed.stdout_text, HasSubstr(job));
    EXPECT_THAT(listed.stdout_text, HasSubstr("needs_user"));

    const proc::RunResult shown = rreport({"jobs", "show", job_in(second.stdout_text)});
    ASSERT_EQ(shown.status.code, 0);
    EXPECT_THAT(shown.stdout_text, HasSubstr("title:   On radium"));
    EXPECT_THAT(shown.stdout_text, HasSubstr(std::format("source:  {}", captions.string())));
    EXPECT_THAT(shown.stdout_text, HasSubstr("stage transcript   found done"));
    EXPECT_THAT(shown.stdout_text, HasSubstr("stage outline      found done"));

    const proc::RunResult resumed = rreport({"resume", job}, no_pod);
    EXPECT_EQ(resumed.status.code, 1);
    EXPECT_THAT(resumed.stdout_text, HasSubstr(std::format("job {} needs you: atomizing needs models", job)));
}

// A job's page needs no pod: it shows what the job has, and says what is
// missing.
TEST_F(Hub, ReportsAJobAsAPageWhereverItStands) {
    const std::vector<std::string> no_pod{"runtime.url=", "runpod.pod_id="};
    const std::filesystem::path captions = home().write_file("On radium.vtt", kCaptions);
    const std::string job = job_in(rreport({"ingest", captions.string()}, no_pod).stdout_text);
    ASSERT_FALSE(job.empty());

    const proc::RunResult reported = rreport({"report", job}, no_pod);
    ASSERT_EQ(reported.status.code, 0) << reported.stderr_text;
    const std::filesystem::path page{reported.stdout_text.substr(0, reported.stdout_text.find('\n'))};
    EXPECT_EQ(page.filename(), job + ".html");
    EXPECT_EQ(page.parent_path().filename(), "reports") << "beside the library";
    const std::string html = unwrap(read_file(page));
    EXPECT_THAT(html, HasSubstr("<title>On radium</title>"));
    EXPECT_THAT(html, HasSubstr("The article is not written yet"));
    EXPECT_THAT(html, HasSubstr("Pierre Curie heated it until it glowed.")) << "its transcript";
    EXPECT_THAT(html, HasSubstr("<h4>outline</h4>")) << "what the outline stage could not do";

    const std::filesystem::path elsewhere = home() / "pages" / "radium.html";
    const proc::RunResult chosen = rreport({"report", job, "--out", elsewhere.string()}, no_pod);
    ASSERT_EQ(chosen.status.code, 0) << chosen.stderr_text;
    EXPECT_EQ(unwrap(read_file(elsewhere)), html) << "a view of the same artifacts, the same each time";

    EXPECT_EQ(rreport({"report", "01JNOSUCHJOB0000000000000000"}, no_pod).status.code, 1);
}

TEST_F(Hub, UnpunctuatedCaptionsWithoutAPodSaySo) {
    const std::filesystem::path captions =
        home().write_file("auto.vtt",
                          "WEBVTT\n\n00:00.000 --> 00:03.000\nso marie curie won two nobel prizes she\n\n"
                          "00:03.000 --> 00:06.000\ncalled the element radium and it glowed\n");
    const proc::RunResult result = rreport({"ingest", captions.string()}, {"runtime.url=", "runpod.pod_id="});
    EXPECT_EQ(result.status.code, 1) << "the atomize stage waits for a pod";
    EXPECT_THAT(result.stdout_text, HasSubstr("what the stages could not do:"));
    EXPECT_THAT(result.stdout_text,
                HasSubstr("transcript: The captions are not punctuated, and no pod is configured"));
}

TEST_F(Hub, AJobThatCannotBeDoneSaysWhy) {
    const proc::RunResult missing = rreport({"ingest", (home() / "missing.vtt").string()});
    EXPECT_EQ(missing.status.code, 1);
    EXPECT_THAT(missing.stdout_text, HasSubstr("failed: cannot open"));
    const proc::RunResult shown = rreport({"jobs", "show", job_in(missing.stdout_text)});
    EXPECT_THAT(shown.stdout_text, HasSubstr("stage source       failed"));

    const proc::RunResult tier = rreport({"ingest", (home() / "missing.vtt").string(), "--tier", "turbo"});
    EXPECT_EQ(tier.status.code, 2) << "an unknown tier is refused before a job is made";
    const proc::RunResult unknown = rreport({"resume", "01J00000000000000000000000"});
    EXPECT_EQ(unknown.status.code, 1);
    EXPECT_THAT(unknown.stderr_text, HasSubstr("rreport jobs"));
}

// --- The eval -----------------------------------------------------------------------------

constexpr std::string_view kSet = R"toml(
[[video]]
id = "radium-talk"
title = "On radium"
url = "https://www.youtube.com/watch?v=abc123XYZ_0"
type = "talk"
captions = "manual"
split = "dev"
licence = "us-public-domain"
attribution = "Science Hour"
)toml";

// yt-dlp, played by a script in `home`: it describes the video of kSet, and
// downloads kCaptions as its English captions. The settings that use it.
std::vector<std::string> fake_ytdlp(const testkit::TempDir& home) {
    (void)home.write_file("ytdlp/info.json", R"json({"id": "abc123XYZ_0", "title": "On radium",
        "channel": "Science Hour", "language": "en", "subtitles": {"en": [{"ext": "vtt"}]},
        "webpage_url": "https://www.youtube.com/watch?v=abc123XYZ_0"})json");
    (void)home.write_file("ytdlp/captions.vtt", kCaptions);
    const std::filesystem::path script =
        home.write_file("ytdlp/yt-dlp", std::format(R"(#!/bin/sh
mode=captions
while [ $# -gt 0 ]; do
  case "$1" in
    --dump-single-json) mode=describe ;;
    --paths) directory="$2" ;;
  esac
  shift
done
if [ "$mode" = describe ]; then cat '{0}/info.json'; else cp '{0}/captions.vtt' "$directory/captions.en.vtt"; fi
)",
                                                    (home / "ytdlp").string()));
    std::filesystem::permissions(script, std::filesystem::perms::owner_all);
    return {std::format("media.ytdlp={}", script.string())};
}

TEST_F(Hub, TheEvalKeepsItsVideosAndDraftsTheirGold) {
    const std::filesystem::path eval = home() / "eval";
    (void)home().write_file("eval/sets/m2.toml", kSet);
    const std::vector<std::string> ytdlp = fake_ytdlp(home());

    const proc::RunResult fetched = rreport({"eval", "--eval-dir", eval.string(), "fetch"}, ytdlp);
    ASSERT_EQ(fetched.status.code, 0) << fetched.stdout_text << fetched.stderr_text;
    EXPECT_THAT(fetched.stdout_text, HasSubstr("radium-talk: kept "));
    EXPECT_THAT(fetched.stdout_text, HasSubstr("pin it in the set, under the video: snapshot = \""));
    EXPECT_TRUE(std::filesystem::exists(eval / "data" / "radium-talk" / "radium-talk.source.json"));
    const proc::RunResult kept =
        rreport({"eval", "--eval-dir", eval.string(), "fetch", "radium-talk"}, ytdlp);
    ASSERT_EQ(kept.status.code, 0);
    EXPECT_THAT(kept.stdout_text, HasSubstr("radium-talk: kept already, snapshot ")) << "read once";

    const proc::RunResult waiting =
        rreport({"eval", "--eval-dir", eval.string(), "annotate"}, {"runtime.url=", "runpod.pod_id="});
    EXPECT_EQ(waiting.status.code, 1);
    EXPECT_THAT(waiting.stdout_text,
                HasSubstr("needs you: drafting an annotation needs models, and no pod is "
                          "configured"));
    EXPECT_THAT(
        waiting.stdout_text,
        HasSubstr(std::format("then: rreport eval annotate --eval-dir {} radium-talk", eval.string())));
    const proc::RunResult listed = rreport({"jobs"});
    EXPECT_THAT(listed.stdout_text, HasSubstr("annotation  needs_user"));
    const proc::RunResult resumed = rreport({"resume", job_in(waiting.stdout_text)});
    EXPECT_EQ(resumed.status.code, 2) << "an annotation runs other stages than an article";
    EXPECT_THAT(resumed.stderr_text, HasSubstr("`rreport eval annotate` takes it up again"));
    const proc::RunResult paged = rreport({"report", job_in(waiting.stdout_text)});
    EXPECT_EQ(paged.status.code, 2) << "an annotation has no article to show";
    EXPECT_THAT(paged.stderr_text, HasSubstr("`rreport eval sheet` shows what it drafted"));

    // The gold file a person corrected.
    const std::string snapshot =
        fetched.stdout_text.substr(fetched.stdout_text.find("snapshot = \"") + 12, 64);
    (void)home().write_file("eval/gold/radium-talk.toml",
                            std::format("video = \"radium-talk\"\nsnapshot = \"{}\"\nstatus = \"gold\"\n"
                                        "reviewer = \"Ada\"\n\n[[fact]]\nid = \"F1\"\nstart = \"0:03\"\n"
                                        "end = \"0:06\"\nimportance = \"core\"\nquote = \"heated it until it "
                                        "glowed\"\nclaim = \"Pierre Curie heated radium until it glowed.\"\n",
                                        snapshot));
    const proc::RunResult sheet = rreport({"eval", "--eval-dir", eval.string(), "sheet", "radium-talk"});
    ASSERT_EQ(sheet.status.code, 0) << sheet.stderr_text;
    EXPECT_THAT(sheet.stdout_text, HasSubstr("# Review: On radium"));
    EXPECT_THAT(sheet.stdout_text,
                HasSubstr("| F1 | [0:03\u20130:06](https://www.youtube.com/watch?v=abc123XYZ_0&t=3s) "
                          "| 0 | core | Pierre Curie heated radium until it glowed. | "
                          "\"heated it until it glowed\" | \u2713 |"));
    EXPECT_THAT(sheet.stdout_text, HasSubstr("Nothing the sheet can check."));
    const std::filesystem::path out = home() / "sheet.md";
    const proc::RunResult saved =
        rreport({"eval", "--eval-dir", eval.string(), "sheet", "radium-talk", "--out", out.string()});
    ASSERT_EQ(saved.status.code, 0);
    EXPECT_EQ(unwrap(read_file(out)), sheet.stdout_text);
}

TEST_F(Hub, AJudgementIsTakenUpByEvalJudgeAndPagedByTheArticleItJudges) {
    std::string article;
    std::string judgement;
    {
        db::Database database =
            unwrap(library::open_database(data_dir() / "libraries" / "default" / "library.db"));
        library::Jobs jobs{database, SystemClock::instance()};
        article = unwrap(jobs.create("talk.vtt", "max", 0)).id;
        judgement = unwrap(jobs.create(article, "eval-v1", 0, library::JobKind::judgement)).id;
    }
    const proc::RunResult resumed = rreport({"resume", judgement});
    EXPECT_EQ(resumed.status.code, 2) << "a judgement runs other stages than an article";
    EXPECT_THAT(resumed.stderr_text,
                HasSubstr("`rreport eval judge` takes it up again, with its run's label"));
    const proc::RunResult paged = rreport({"report", judgement});
    EXPECT_EQ(paged.status.code, 2) << "a judgement has no article of its own";
    EXPECT_THAT(paged.stderr_text, HasSubstr(std::format("`rreport report {}` pages that one", article)));
    EXPECT_THAT(rreport({"jobs"}).stdout_text, HasSubstr("judgement"));
}

TEST_F(Hub, APairingIsTakenUpByEvalCompareAndHasNoPageOfItsOwn) {
    std::string pairing;
    {
        db::Database database =
            unwrap(library::open_database(data_dir() / "libraries" / "default" / "library.db"));
        library::Jobs jobs{database, SystemClock::instance()};
        const std::string a = unwrap(jobs.create("talk.vtt", "max", 0)).id;
        const std::string b = unwrap(jobs.create("talk.vtt", "fast", 0)).id;
        pairing = unwrap(jobs.create(std::format("{} {}", a, b), "eval-v1", 0, library::JobKind::pairing)).id;
    }
    const proc::RunResult resumed = rreport({"resume", pairing});
    EXPECT_EQ(resumed.status.code, 2) << "a pairing runs other stages than an article";
    EXPECT_THAT(resumed.stderr_text,
                HasSubstr("`rreport eval compare --flow` takes it up again, with its runs' labels"));
    const proc::RunResult paged = rreport({"report", pairing});
    EXPECT_EQ(paged.status.code, 2) << "a pairing has no article of its own";
    EXPECT_THAT(paged.stderr_text, HasSubstr("has none of its own: `rreport report` pages each"));
    EXPECT_THAT(rreport({"jobs"}).stdout_text, HasSubstr("pairing"));
}

// A gate of one invariant and one margin, and a panel to read judged metrics
// by, as a person would write them.
constexpr std::string_view kGate = R"toml(name = "g"
[[invariant]]
metric = "invalid_markers"
every_entry = 0
[[gated]]
metric = "atoms_told"
margin = 0.01
)toml";

constexpr std::string_view kPanel = R"toml(name = "p"
procedure = 1
calibrated = false
[[prompt]]
name = "eval_fact"
version = 1
hash = "f"
[[prompt]]
name = "eval_support"
version = 1
hash = "s"
[[prompt]]
name = "eval_flow"
version = 1
hash = "w"
[[judge]]
role = "commander"
model = "chat"
sha256 = "c600de0300ae8a0eb3a6c0b8b5561b8b96f16bd2c863c2a66c42de29d391a747"
)toml";

// A panel frozen for this rreport's prompts: it reads, once its judges are served.
std::string frozen_panel() {
    const prompt::Library prompts = unwrap(prompt::Library::embedded());
    std::string text =
        std::format("name = \"frozen\"\nprocedure = {}\ncalibrated = false\n", eval::kJudgingProcedure);
    for (const std::string_view name : eval::kJudgePrompts) {
        const prompt::Version& current = prompts.find(name)->current();
        text += std::format("[[prompt]]\nname = \"{}\"\nversion = {}\nhash = \"{}\"\n", name, current.number,
                            current.hash);
    }
    return text +
           "[[judge]]\nrole = \"commander\"\nmodel = \"chat\"\nsha256 = "
           "\"c600de0300ae8a0eb3a6c0b8b5561b8b96f16bd2c863c2a66c42de29d391a747\"\n";
}

TEST_F(Hub, TwoRunsAreComparedAndTheGateDecidesOnlyWhatItsNumbersCanSay) {
    const std::filesystem::path eval_dir = home() / "eval";
    (void)home().write_file("eval/gates/g.toml", kGate);
    (void)home().write_file("eval/judges/p.toml", kPanel);
    {
        // Three runs of two videos, two replicates each, as `eval run` and
        // `eval score` would leave them: the baseline; a candidate whose
        // podcast's second replicate writes a marker of no atom; and the
        // baseline again under other salts, scoring as it did.
        db::Database database =
            unwrap(library::open_database(data_dir() / "libraries" / "default" / "library.db"));
        library::Jobs jobs{database, SystemClock::instance()};
        eval::Runs runs{database, SystemClock::instance()};
        const auto run = [&](std::string label, int first, bool broken) {
            const eval::EvalRun made = unwrap(runs.open(eval::RunSpec{.label = std::move(label),
                                                                      .eval_set = "m2",
                                                                      .tier = "max",
                                                                      .replicates = 2,
                                                                      .first_replicate = first}));
            for (const std::string video : {"lecture", "podcast"}) {
                for (int replicate = first; replicate < first + 2; ++replicate) {
                    const eval::RunEntry entry{.video = video,
                                               .replicate = replicate,
                                               .job = unwrap(jobs.create("talk.vtt", "max", replicate)).id};
                    unwrap(runs.add(made.id, entry));
                    const bool marked = broken && video == "podcast" && replicate == first + 1;
                    unwrap(runs.keep(made.id, entry,
                                     eval::Scoring{.scorer = "code@1",
                                                   .scores = {{.metric = "atoms_told", .value = 0.9},
                                                              {.metric = "invalid_markers",
                                                               .value = marked ? 1.0 : 0.0}}}));
                }
            }
        };
        run("base", 0, false);
        run("cand", 0, true);
        run("base-aa", 2, false);
    }
    const auto eval = [&](std::vector<std::string> command) {
        command.insert(command.begin(), {"eval", "--eval-dir", eval_dir.string()});
        return rreport(command);
    };

    const proc::RunResult compared = eval({"compare", "base", "cand", "--gate", "g"});
    ASSERT_EQ(compared.status.code, 0) << compared.stderr_text;
    EXPECT_THAT(compared.stdout_text, HasSubstr("A: base (m2 dev, tier max, replicates 0 to 1)\n"));
    EXPECT_THAT(compared.stdout_text, HasSubstr("\ncode@1\n"));
    EXPECT_THAT(compared.stdout_text, HasSubstr("  invalid_markers"));
    const proc::RunResult by_video =
        eval({"compare", "base", "cand", "--gate", "g", "--metric", "invalid_markers"});
    EXPECT_THAT(by_video.stdout_text,
                HasSubstr("  podcast                          0.000     0.500    +0.500\n"))
        << "a video's replicates averaged";
    EXPECT_EQ(eval({"compare", "base", "nope", "--gate", "g"}).status.code, 2) << "no such run";
    EXPECT_EQ(eval({"compare", "base", "cand", "--gate", "g", "--metric", "nope"}).status.code, 2);

    const proc::RunResult gated = eval({"gate", "base", "cand", "--gate", "g", "--panel", "p"});
    EXPECT_EQ(gated.status.code, 1) << gated.stdout_text << gated.stderr_text;
    EXPECT_THAT(gated.stdout_text, HasSubstr("failed: broken by podcast, replicate 1: 1.000"));
    EXPECT_THAT(gated.stdout_text, HasSubstr("\ngate g: failed\n"));
    const proc::RunResult same = eval({"gate", "base", "base-aa", "--gate", "g", "--panel", "p"});
    EXPECT_EQ(same.status.code, 0) << same.stdout_text;
    EXPECT_THAT(same.stdout_text, HasSubstr("\ngate g: passed\n"));
    EXPECT_EQ(eval({"gate", "base", "cand", "--gate", "missing", "--panel", "p"}).status.code, 2);
    EXPECT_EQ(eval({"gate", "base", "cand", "--gate", "g", "--panel", "missing"}).status.code, 2);

    const proc::RunResult shared = eval({"gate", "base", "cand", "--aa", "--gate", "g", "--panel", "p"});
    EXPECT_EQ(shared.status.code, 2);
    EXPECT_THAT(shared.stderr_text, HasSubstr("cand shares salts with base"));
    const proc::RunResult aa = eval({"gate", "base", "base-aa", "--aa", "--gate", "g", "--panel", "p"});
    EXPECT_EQ(aa.status.code, 0) << aa.stdout_text << aa.stderr_text;
    EXPECT_THAT(aa.stdout_text, HasSubstr("gate g: its margins hold\n"));

    const proc::RunResult flow_alone = eval({"compare", "base", "cand", "--gate", "g", "--metric", "flow"});
    EXPECT_EQ(flow_alone.status.code, 2);
    EXPECT_THAT(flow_alone.stderr_text,
                HasSubstr("flow is read pairwise, between the two runs' articles: add --flow"));
    const proc::RunResult stale = eval({"compare", "base", "cand", "--gate", "g", "--flow", "--panel", "p"});
    EXPECT_EQ(stale.status.code, 2) << "a panel frozen for other prompts reads nothing";
    EXPECT_THAT(stale.stderr_text, Not(HasSubstr("flow: lecture"))) << "no pairing started";

    (void)home().write_file("eval/judges/frozen.toml", frozen_panel());
    const proc::RunResult unread =
        eval({"compare", "base", "cand", "--gate", "g", "--flow", "--panel", "frozen"});
    EXPECT_EQ(unread.status.code, 1) << "pairs of jobs not done, not read" << unread.stderr_text;
    EXPECT_THAT(unread.stdout_text,
                HasSubstr("\nwhat the flow could not read:\n  not paired: lecture, replicates 0 "
                          "and 0: job "));
    EXPECT_THAT(unread.stdout_text, HasSubstr(" is running\n"));
    EXPECT_THAT(unread.stdout_text,
                HasSubstr("  flow                          0         -         -         -   -"))
        << "a row, of no video";
}

TEST_F(Hub, TheEvalSaysWhatIsMissing) {
    const std::filesystem::path eval = home() / "eval";
    const proc::RunResult no_set = rreport({"eval", "--eval-dir", eval.string(), "fetch"});
    EXPECT_EQ(no_set.status.code, 2);
    EXPECT_THAT(no_set.stderr_text, HasSubstr("a set is made from the approved candidates"));

    (void)home().write_file("eval/sets/m2.toml", kSet);
    const proc::RunResult unknown = rreport({"eval", "--eval-dir", eval.string(), "annotate", "polonium"});
    EXPECT_EQ(unknown.status.code, 2);
    EXPECT_THAT(unknown.stderr_text, HasSubstr("the set 'm2' has no video 'polonium'"));

    const proc::RunResult unfetched = rreport({"eval", "--eval-dir", eval.string(), "annotate"});
    EXPECT_EQ(unfetched.status.code, 1);
    EXPECT_THAT(unfetched.stderr_text, HasSubstr("`rreport eval fetch radium-talk` reads it first"));
    const proc::RunResult unsheeted = rreport({"eval", "--eval-dir", eval.string(), "sheet", "radium-talk"});
    EXPECT_EQ(unsheeted.status.code, 1);
    EXPECT_THAT(unsheeted.stderr_text,
                HasSubstr(std::format("has no snapshot yet: rreport eval fetch --eval-dir {} "
                                      "radium-talk reads it",
                                      eval.string())));
}

TEST_F(Hub, ConfigurationMistakesAndSecrets) {
    const proc::RunResult typo = rreport({"doctor"}, {"runtime.tokn=x"});
    EXPECT_EQ(typo.status.code, 2);
    EXPECT_THAT(typo.stderr_text, HasSubstr("did you mean 'runtime.token'"));

    const proc::RunResult replayed = rreport({"bench"}, {"llm.mode=replay"});
    EXPECT_EQ(replayed.status.code, 2);
    EXPECT_THAT(replayed.stderr_text, HasSubstr("run it with llm.mode = live"))
        << "a replay measures nothing";

    const proc::RunResult shown = rreport({"config", "show", "--origin"});
    EXPECT_EQ(shown.status.code, 0);
    EXPECT_THAT(shown.stdout_text, HasSubstr("RREPORT_RUNTIME__TOKEN")) << "where the token came from";
    EXPECT_THAT(shown.stdout_text, Not(HasSubstr(std::string{component::kRuntimeToken})))
        << "never the token itself";
    EXPECT_THAT(shown.stdout_text, Not(HasSubstr(std::string{FakeRunpod::kAccountKey})));

    const proc::RunResult version = rreport({"version"});
    EXPECT_EQ(version.status.code, 0);
    EXPECT_THAT(version.stdout_text, HasSubstr("rreport 0."));
}

}  // namespace
}  // namespace rr
