// What every stage that asks models says the same way (rr/pipeline/asking.hpp).
#include <rr/pipeline/asking.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

namespace rr::pipeline {

namespace {

constexpr std::string_view kCut = "\u2026";  // where a description was cut

}  // namespace

Result<StagePrompt> stage_prompt(const prompt::Library& prompts, std::string_view name) {
    const prompt::PromptInfo* info = prompts.find(name);
    if (info == nullptr) {
        return fail(Errc::not_found, std::format("the prompts have no '{}'", name));
    }
    RR_TRY_ASSIGN(std::shared_ptr<const json::Schema> schema, prompts.answer_schema(name));
    return StagePrompt{.info = info, .schema = std::move(schema)};
}

Result<json::Json> prompts_fingerprint(const prompt::Library& prompts,
                                       std::initializer_list<std::string_view> names) {
    json::Json fingerprint = json::Json::object();
    for (const std::string_view name : names) {
        RR_TRY_ASSIGN(const StagePrompt prompt, stage_prompt(prompts, name));
        RR_TRY_ASSIGN(const std::string hash, prompts.content_hash(name));
        fingerprint[std::string{name}] = json::Json{{"label", prompt.info->label()}, {"hash", hash}};
    }
    return fingerprint;
}

const agents::Engine* engine_serving(const agents::Army& army, std::string_view role) {
    const auto found = std::ranges::find_if(army.engines(), [&](const agents::Engine& engine) {
        return std::ranges::contains(engine.roles, role);
    });
    return found == army.engines().end() ? nullptr : &*found;
}

std::string description_excerpt(std::string_view description) {
    if (description.size() <= kDescriptionBytes) {
        return std::string{description};
    }
    const std::string_view head = description.substr(0, kDescriptionBytes);
    std::size_t kept = 0;
    if (const std::size_t line = head.rfind('\n'); line != std::string_view::npos && line > 0) {
        kept = line + 1;
    } else if (const std::size_t word = head.rfind(' '); word != std::string_view::npos && word > 0) {
        kept = word;
    } else {
        // No space in its first bytes: it is cut before a character, never inside one.
        kept = kDescriptionBytes;
        while (kept > 0 && (static_cast<unsigned char>(description[kept]) & 0xC0U) == 0x80U) {
            --kept;
        }
    }
    std::string cut{head.substr(0, kept)};
    cut += kCut;
    return cut;
}

json::Json video_shared(const media::Video& video) {
    return json::Json{{"title", video.title},
                      {"channel", video.channel},
                      {"description", description_excerpt(video.description)}};
}

std::string why_unanswered(const agents::Outcome& outcome, const agents::Army& army, std::string_view task) {
    switch (outcome.verdict) {
        case agents::Verdict::refused:
            return std::format("the models declined to {}", task);
        case agents::Verdict::ceiling: {
            const std::optional<agents::CeilingHit> hit = army.ceiling();
            return std::format("the job reached its {} ceiling first",
                               hit.has_value() ? agents::to_string(hit->kind) : std::string_view{"spending"});
        }
        case agents::Verdict::accepted:
        case agents::Verdict::invalid:
            break;
    }
    std::string problems;
    for (const std::string& problem : outcome.problems) {
        problems += problems.empty() ? problem : "; " + problem;
    }
    return std::format("no answer passed its checks ({})", problems);
}

}  // namespace rr::pipeline
