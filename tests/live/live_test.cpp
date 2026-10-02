// L4: rReport's runtime against a real llama-server.
//
// Everywhere else rr_fake_engine stands in for llama-server. These tests run
// the real one, built for the CPU at the image's pinned llama.cpp, behind a
// real rreport-runtime, and talk to it through the hub's own client. The
// model is tiny, with random weights (scripts/tiny-model.py): what is checked
// is the server's mechanics, which the fake, the catalog and the runtime's
// documentation describe:
//   * each engine's context per slot: an equal share, a shared pool, and the
//     model's trained context capping both (runtime.md);
//   * a grammar keeps a schema's fields in the order written (llm.md, P1);
//   * a request for n answers reports each one's usage, summed (P3);
//   * a thinking budget ends the thinking, and the model answers (P2);
//   * each token's probability, after the sampler chain, comes with it,
//     and an answer's tokens cover its text byte for byte (P28);
//   * a shared system message is served from the cache, as the pipeline's
//     prompts rely on (P5's bench);
//   * a prompt beyond a slot's share is refused before it starts, and
//     requests that together overflow a shared pool all fail while the
//     engine carries on (runtime.md, runbook R1).
// The nightly job (.github/workflows/nightly.yml) sets RR_LIVE_LLAMA_SERVER
// and RR_LIVE_MODEL. Without them every test is skipped, so the suite builds
// and runs everywhere and cannot rot.
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/core/hash.hpp>
#include <rr/json/json.hpp>
#include <rr/llm/chat.hpp>
#include <rr/net/http_client.hpp>
#include <rr/proc/process.hpp>
#include <rr/runtime_client/runtime_client.hpp>
#include <rr/testkit/ports.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr {
namespace {

using namespace std::chrono_literals;
using ::testing::HasSubstr;
using testkit::unwrap;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kToken = "live-token";

// An environment variable, or nothing when it is unset or empty.
std::optional<std::string> variable(const char* name) {
    const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): no thread runs yet
    return value == nullptr || *value == '\0' ? std::nullopt : std::optional<std::string>{value};
}

// The real server and model, from the environment; nothing when either is unset.
struct Live {
    std::string llama_server;
    std::filesystem::path model;
};

std::optional<Live> live() {
    std::optional<std::string> server = variable("RR_LIVE_LLAMA_SERVER");
    std::optional<std::string> model = variable("RR_LIVE_MODEL");
    if (!server.has_value() || !model.has_value()) {
        return std::nullopt;
    }
    return Live{.llama_server = std::move(*server), .model = std::move(*model)};
}

// rreport-runtime with three engines of the tiny model, one per way a slot
// gets its context:
//   pool    2 slots sharing 2048 tokens: either may use all of them;
//   split   2 slots, 2048 split evenly: 1024 each;
//   capped  1 slot of 16384, beyond the model's trained 8192: capped there.
class Runtime {
public:
    explicit Runtime(const Live& setup) {
        const std::string sha = unwrap(sha256_file(setup.model)).hex();
        for (const std::string_view name : {"pool", "split", "capped"}) {
            std::filesystem::create_directories(dir_ / "models");
            std::filesystem::copy_file(setup.model, dir_ / "models" / std::format("{}.gguf", name));
        }
        (void)dir_.write_file("catalog.toml", std::format(R"toml(
[[model]]
id = "pool"
role = "commander"
file = "pool.gguf"
sha256 = "{0}"
[[model]]
id = "split"
role = "infantry"
file = "split.gguf"
sha256 = "{0}"
[[model]]
id = "capped"
role = "infantry"
file = "capped.gguf"
sha256 = "{0}"

[[profile]]
name = "live"
  [[profile.engine]]
  model = "pool"
  slots = 2
  context = 2048
  kv_unified = true
  extra_args = ["--jinja", "--threads", "1"]
  [[profile.engine]]
  model = "split"
  slots = 2
  context = 2048
  extra_args = ["--jinja", "--threads", "1"]
  [[profile.engine]]
  model = "capped"
  context = 16384
  extra_args = ["--jinja", "--threads", "1"]
)toml",
                                                          sha));
        model_sha256_ = sha;
        std::vector<std::string> argv{RR_RUNTIME_BINARY, "serve"};
        for (const std::string& setting :
             {std::string{"server.host=127.0.0.1"}, std::string{"server.port=0"},
              std::format("models.catalog={}", (dir_ / "catalog.toml").string()),
              std::format("models.dir={}", (dir_ / "models").string()), std::string{"models.profile=live"},
              std::format("blobs.dir={}", (dir_ / "blobs").string()),
              std::format("engines.llama_server={}", setup.llama_server),
              std::format("engines.first_port={}", testkit::free_port_range(3)),
              std::string{"idle.stop_after=0s"}, std::string{"log.level=warn"}}) {
            argv.emplace_back("--set");
            argv.push_back(setting);
        }
        process_.emplace(unwrap(proc::ChildProcess::start(
            proc::Command{.argv = std::move(argv),
                          .environment = {{"RR_RUNTIME_TOKEN", std::string{kToken}}}},
            proc::ChildProcess::Options{.on_output = [this](std::string_view line) {
                constexpr std::string_view kListening = "listening on 127.0.0.1:";
                if (line.starts_with(kListening)) {
                    const std::lock_guard lock(mutex_);
                    port_ = std::stoi(std::string{line.substr(kListening.size())});
                }
            }})));
        const auto deadline = Clock::now() + 60s;
        while (port() == 0 && Clock::now() < deadline) {
            std::this_thread::sleep_for(20ms);
        }
        if (port() == 0) {
            throw std::runtime_error("the runtime did not start");
        }
    }
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;
    ~Runtime() {
        if (process_.has_value()) {
            (void)::kill(process_->pid(), SIGTERM);
            (void)process_->wait(30s);
        }
    }

    [[nodiscard]] runtime_client::Endpoint endpoint() const {
        return {.url = std::format("http://127.0.0.1:{}", port()), .token = Secret{std::string{kToken}}};
    }
    [[nodiscard]] const std::string& model_sha256() const { return model_sha256_; }

private:
    [[nodiscard]] int port() const {
        const std::lock_guard lock(mutex_);
        return port_;
    }

    testkit::TempDir dir_{"rr_live_runtime"};
    std::string model_sha256_;
    mutable std::mutex mutex_;
    int port_ = 0;
    std::optional<proc::ChildProcess> process_;
};

// About `count` tokens: Qwen's tokenizer reads each " the" as one.
std::string filler(int count) {
    std::string text;
    for (int index = 0; index < count; ++index) {
        text += " the";
    }
    return text;
}

// Greedy, short, thinking off: what most checks need.
llm::ChatRequest request(std::string model, std::string prompt, int max_tokens) {
    return {.model = std::move(model),
            .messages = {llm::Message::user(std::move(prompt))},
            .max_tokens = max_tokens,
            .temperature = 0.0,
            .seed = 1,
            .thinking = false,
            .tag = {.purpose = "live"}};
}

// A runtime of its own for each test, so that none sees another's traces
// (a prompt in the cache, an overflowed pool). On the tiny model its engines
// start in a second or two.
class LiveTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::optional<Live> setup = live();
        if (!setup.has_value()) {
            GTEST_SKIP() << "set RR_LIVE_LLAMA_SERVER and RR_LIVE_MODEL to run against a real llama-server";
        }
        runtime_.emplace(*setup);
        const runtime_client::RuntimeClient client{http_, runtime_->endpoint()};
        health_ = unwrap(client.wait_until_ready(5min));
        provider_ = runtime_client::runtime_provider(http_, runtime_->endpoint());
    }

    [[nodiscard]] const llm::LlmProvider& models() const { return *provider_; }
    [[nodiscard]] const Runtime& runtime() const { return *runtime_; }

    [[nodiscard]] const runtime_api::EngineInfo& engine(std::string_view model) const {
        for (const runtime_api::EngineInfo& info : health_.engines) {
            if (info.model == model) {
                return info;
            }
        }
        throw std::runtime_error(std::format("no engine '{}'", model));
    }

private:
    net::HttpClient http_;
    std::optional<Runtime> runtime_;
    runtime_api::HealthReport health_;
    std::unique_ptr<llm::LlmProvider> provider_;  // last: it calls through http_
};

TEST_F(LiveTest, EachEngineReportsTheContextLlamaServerGivesItsSlots) {
    EXPECT_EQ(engine("pool").ctx_per_slot, 2048) << "a shared pool: one sequence may use all of it";
    EXPECT_TRUE(engine("pool").kv_unified);
    EXPECT_EQ(engine("split").ctx_per_slot, 1024) << "an equal share, as the catalog's arithmetic says";
    EXPECT_EQ(engine("capped").ctx_per_slot, 8192)
        << "llama-server caps the catalog's 16384 at the model's trained context";
    EXPECT_EQ(engine("capped").context, 16384) << "the pool itself is the catalog's";
    EXPECT_EQ(engine("split").model_sha256, runtime().model_sha256());
}

TEST_F(LiveTest, AGrammarKeepsTheFieldsInTheOrderWritten) {
    llm::ChatRequest asked = request("split", "Answer in JSON.", 120);
    // zeta before alpha: the order an alphabetical sort would reverse.
    asked.json_schema =
        R"({"type":"object","properties":{"zeta":{"type":"integer"},"alpha":{"type":"boolean"}},)"
        R"("required":["zeta","alpha"],"additionalProperties":false})";
    const llm::ChatResponse answer = unwrap(models().chat(asked, {}, {}));
    const json::OrderedJson document = unwrap(json::parse_ordered(answer.first().content));
    ASSERT_TRUE(document.is_object()) << answer.first().content;
    ASSERT_EQ(document.size(), 2U) << answer.first().content;
    EXPECT_EQ(document.begin().key(), "zeta") << answer.first().content;
}

TEST_F(LiveTest, SeveralAnswersReportTheirOwnUsage) {
    llm::ChatRequest asked = request("pool", "Say something.", 6);
    asked.n = 2;
    asked.temperature = 1.0;
    asked.extra = {{"ignore_eos", true}};
    const llm::ChatResponse answer = unwrap(models().chat(asked, {}, {}));
    ASSERT_EQ(answer.choices.size(), 2U);
    for (const llm::Choice& choice : answer.choices) {
        EXPECT_EQ(choice.completion_tokens, 6);
        EXPECT_FALSE(choice.content.empty());
    }
    EXPECT_EQ(answer.usage.completion_tokens, 12) << "summed over the answers";
    EXPECT_GT(answer.usage.prompt_tokens, 0);
}

// rr_llm places each token's probability on the bytes its chunk added
// (rr/llm/openai_provider.hpp). On an answer of ASCII words held to a
// schema, every token's text reaches the content, so the tokens must cover
// it byte for byte, in order: what the fake engine assumes of llama-server.
TEST_F(LiveTest, EachTokensProbabilityComesWithItAndTheTokensCoverTheAnswer) {
    llm::ChatRequest asked = request("pool", "Classify the sentence.", 40);
    asked.json_schema =
        R"({"type":"object","properties":{"kind":{"enum":["claim","fact","joke"]},)"
        R"("sure":{"type":"boolean"}},"required":["kind","sure"],"additionalProperties":false})";
    asked.token_probabilities = true;
    asked.n = 2;
    asked.temperature = 1.0;  // the sampler chain decides: probabilities below 1 too
    const llm::ChatResponse answer = unwrap(models().chat(asked, {}, {}));
    ASSERT_EQ(answer.choices.size(), 2U);
    for (const llm::Choice& choice : answer.choices) {
        ASSERT_FALSE(choice.probabilities.empty()) << choice.content;
        std::size_t covered = 0;
        for (const llm::TokenProbability& token : choice.probabilities) {
            EXPECT_EQ(token.begin, covered) << choice.content;
            EXPECT_GT(token.probability, 0.0) << "the sampled token was among the chain's candidates";
            EXPECT_LE(token.probability, 1.0);
            covered = token.end;
        }
        EXPECT_EQ(covered, choice.content.size()) << choice.content;
    }
}

// The model never closes its thinking by itself (its weights are random): an
// answer after the thinking is the budget's doing.
TEST_F(LiveTest, AThinkingBudgetEndsTheThinkingAndTheModelAnswers) {
    llm::ChatRequest budgeted = request("split", "Think, then answer.", 40);
    budgeted.thinking = true;
    budgeted.thinking_budget = llm::ThinkingBudget{.tokens = 8};
    budgeted.extra = {{"ignore_eos", true}};
    const llm::ChatResponse cut = unwrap(models().chat(budgeted, {}, {}));
    EXPECT_FALSE(cut.first().reasoning.empty());
    EXPECT_FALSE(cut.first().content.empty()) << "the thinking ended within the 40 tokens";

    llm::ChatRequest unbounded = budgeted;
    unbounded.thinking_budget.reset();
    const llm::ChatResponse endless = unwrap(models().chat(unbounded, {}, {}));
    EXPECT_FALSE(endless.first().reasoning.empty());
    EXPECT_TRUE(endless.first().content.empty()) << "without a budget, it thinks to the end";
}

TEST_F(LiveTest, ASharedSystemMessageIsServedFromTheCache) {
    const std::string shared = "You read records." + filler(600);
    const auto asking = [&](std::string question) {
        llm::ChatRequest asked = request("split", std::move(question), 2);
        asked.messages.insert(asked.messages.begin(), llm::Message::system(shared));
        return asked;
    };
    const llm::ChatResponse first = unwrap(models().chat(asking("Question one?"), {}, {}));
    const llm::ChatResponse next = unwrap(models().chat(asking("Question two?"), {}, {}));
    EXPECT_GE(next.usage.cached_prompt_tokens, 600)
        << "of " << next.usage.prompt_tokens << " (the first had " << first.usage.cached_prompt_tokens << ")";
}

TEST_F(LiveTest, APromptBeyondASlotsShareIsRefusedBeforeItStarts) {
    const auto refused = models().chat(request("split", filler(1500), 4), {}, {});
    ASSERT_FALSE(refused.has_value()) << "1500 tokens in a slot of 1024";
    EXPECT_THAT(refused.error().message(), HasSubstr("exceeds the available context size"));
    EXPECT_OK(models().chat(request("split", filler(500), 4), {}, {})) << "and the engine carries on";
}

TEST_F(LiveTest, RequestsThatOverflowASharedPoolAllFailAndTheEngineCarriesOn) {
    // Each fits the pool alone (1200 + 400 of 2048); together they cannot.
    std::vector<Result<llm::ChatResponse>> answers(2, fail(Errc::internal, "not run"));
    {
        std::latch start{static_cast<std::ptrdiff_t>(answers.size())};  // they set off together
        std::vector<std::jthread> requests;
        requests.reserve(answers.size());
        for (std::size_t index = 0; index < answers.size(); ++index) {
            requests.emplace_back([&, index] {
                llm::ChatRequest asked =
                    request("pool", std::format("Request {}.{}", index, filler(1200)), 400);
                asked.extra = {{"ignore_eos", true}};
                start.arrive_and_wait();
                answers[index] = models().chat(asked, {}, {});
            });
        }
    }
    for (const Result<llm::ChatResponse>& answer : answers) {
        ASSERT_FALSE(answer.has_value()) << "both fail, not only the one that crossed the limit";
        EXPECT_THAT(answer.error().message(), HasSubstr("Context size has been exceeded"));
    }
    llm::ChatRequest alone = request("pool", filler(1200), 400);
    alone.extra = {{"ignore_eos", true}};
    EXPECT_OK(models().chat(alone, {}, {})) << "llama-server carries on: no restart";
}

}  // namespace
}  // namespace rr
