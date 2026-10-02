// fixtures.hpp — what the eval's tests share: a video of the set, its source
// as the source stage reads it, transcripts and captions written sentence by
// sentence, and an army whose models are the oracle.
#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/agents/army.hpp>
#include <rr/core/clock.hpp>
#include <rr/core/time_span.hpp>
#include <rr/eval/set.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/oracle/answers.hpp>
#include <rr/oracle/oracle.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/pipeline/stage.hpp>
#include <rr/testkit/result.hpp>

namespace rr::eval::fixtures {

inline constexpr std::string_view kUrl = "https://www.youtube.com/watch?v=abc123XYZ_0";
inline constexpr std::string_view kCreativeCommons = "Creative Commons Attribution license (reuse allowed)";

// A video of the set: a talk with manual captions, in the US public domain.
inline EvalVideo video() {
    return EvalVideo{.id = "radium-talk",
                     .title = "On radium",
                     .url = std::string{kUrl},
                     .type = VideoType::talk,
                     .captions = media::CaptionKind::manual,
                     .split = Split::dev,
                     .licence = Licence::us_public_domain,
                     .attribution = "Science Hour"};
}

// One sentence, as said: its words, and when.
struct Said {
    std::string text{};
    Millis start{0};
    Millis end{0};
};

// The transcript of `said`: S1, S2, … in its order.
inline knowledge::Transcript transcript_of(const std::vector<Said>& said) {
    knowledge::Transcript transcript;
    transcript.sentences.reserve(said.size());
    for (std::size_t index = 0; index < said.size(); ++index) {
        transcript.sentences.push_back(
            knowledge::Sentence{.id = knowledge::SentenceId{static_cast<std::uint32_t>(index + 1)},
                                .text = said[index].text,
                                .span = testkit::unwrap(TimeSpan::make(said[index].start, said[index].end))});
    }
    return transcript;
}

// WebVTT captions of `said`: a cue a sentence.
inline std::string vtt_of(const std::vector<Said>& said) {
    std::string vtt = "WEBVTT\n";
    const auto at = [](Millis time) {
        const std::int64_t millis = time.count();
        return std::format("{:02}:{:02}:{:02}.{:03}", millis / 3'600'000, (millis / 60'000) % 60,
                           (millis / 1000) % 60, millis % 1000);
    };
    for (const Said& sentence : said) {
        vtt += std::format("\n{} --> {}\n{}\n", at(sentence.start), at(sentence.end), sentence.text);
    }
    return vtt;
}

// The source of video() as the source stage reads it: `vtt`, its creator's
// captions, and YouTube's description of it, with `license`.
inline pipeline::Source source_of(std::string vtt,
                                  std::optional<std::string> license = std::string{kCreativeCommons}) {
    return pipeline::Source{.origin = pipeline::Origin::web,
                            .video = media::Video{.id = "abc123XYZ_0",
                                                  .url = std::string{kUrl},
                                                  .title = "On radium",
                                                  .description = "A lecture on Marie Curie's work.",
                                                  .channel = "Science Hour",
                                                  .license = std::move(license)},
                            .captions = pipeline::SourceCaptions{.language = "en",
                                                                 .kind = media::CaptionKind::manual,
                                                                 .format = transcript::CaptionFormat::vtt,
                                                                 .content = std::move(vtt)}};
}

// The engines of a small pod: the commander thinks within 500 tokens, and
// holds three answers to one prompt at once.
inline std::vector<agents::Engine> engines(int commander_holds = 8192, int infantry_holds = 4096) {
    const llm::SamplingPreset direct{.temperature = 1.0, .top_p = 0.95, .top_k = 64};
    const llm::SamplingPreset thinking{
        .temperature = 1.0,
        .top_p = 0.95,
        .top_k = 64,
        .thinking = true,
        .thinking_budget = llm::ThinkingBudget{.tokens = 500, .closing = "Answer now."}};
    return {agents::Engine{.model = "qwen-commander",
                           .checksum = "sha-q",
                           .roles = {"commander"},
                           .family = "qwen",
                           .slots = 4,
                           .context = commander_holds * 4,
                           .ctx_per_slot = commander_holds,
                           .direct = direct,
                           .thinking = thinking},
            agents::Engine{.model = "gemma-infantry",
                           .checksum = "sha-g",
                           .roles = {"infantry"},
                           .family = "gemma",
                           .slots = 4,
                           .context = infantry_holds * 4,
                           .ctx_per_slot = infantry_holds,
                           .direct = direct,
                           .thinking = thinking}};
}

// The files eval/judges/eval-v1.toml pins: the commander's and the infantry's.
inline constexpr std::string_view kQwenFile =
    "c600de0300ae8a0eb3a6c0b8b5561b8b96f16bd2c863c2a66c42de29d391a747";
inline constexpr std::string_view kGemmaFile =
    "88f4a13b0bb95f031a7fad973e10854122fb67ebc34d214d39a2f65053046abc";

// engines(), serving the files `qwen` and `gemma`.
inline std::vector<agents::Engine> engines_serving(std::string_view qwen = kQwenFile,
                                                   std::string_view gemma = kGemmaFile,
                                                   int commander_holds = 8192, int infantry_holds = 4096) {
    std::vector<agents::Engine> pod = engines(commander_holds, infantry_holds);
    pod.at(0).checksum = std::string{qwen};
    pod.at(1).checksum = std::string{gemma};
    return pod;
}

// An army whose models are the oracle (rr/oracle/answers.hpp), with
// `faults` made where they say, and every request kept.
struct Rig {
    explicit Rig(std::vector<oracle::Fault> faults = {}, std::vector<agents::Engine> pod = engines(),
                 agents::Ceilings ceilings = {})
        : faulty(oracle, std::move(faults))
        , log(faulty) {
        oracle::teach_stages(oracle);
        army.emplace(testkit::unwrap(agents::Army::create(
            agents::Army::Options{.engines = std::move(pod), .job_id = "J1", .ceilings = ceilings}, log,
            clock)));
    }

    pipeline::RaiseArmy raise() {
        return [this]() -> Result<agents::Army*> { return &*army; };
    }

    // The requests of the missions of `kind`, in the order sent.
    [[nodiscard]] std::vector<llm::ChatRequest> requests(std::string_view kind) const {
        std::vector<llm::ChatRequest> found;
        for (llm::ChatRequest& request : log.requests()) {
            if (request.tag.purpose == kind) {
                found.push_back(std::move(request));
            }
        }
        return found;
    }

    oracle::Oracle oracle;
    oracle::FaultyProvider faulty;
    oracle::RequestLog log;
    FakeClock clock;
    std::optional<agents::Army> army;
};

}  // namespace rr::eval::fixtures
