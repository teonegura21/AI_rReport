// A draft's sentences read against their atoms (rr/pipeline/judging.hpp).
#include <rr/pipeline/judging.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <utility>

#include <rr/knowledge/article.hpp>
#include <rr/llm/memo.hpp>
#include <rr/pipeline/asking.hpp>
#include <rr/pipeline/drafting.hpp>
#include <rr/text/span.hpp>

namespace rr::pipeline {

namespace {

struct IssueName {
    Issue issue;
    std::string_view name;
    std::string_view reading;
};

constexpr auto kIssues = std::to_array<IssueName>({
    {.issue = Issue::none, .name = "none", .reading = "saying what its atoms say"},
    {.issue = Issue::adds, .name = "adds", .reading = "adding what its atoms do not say"},
    {.issue = Issue::softens, .name = "softens", .reading = "softening what its atoms say"},
    {.issue = Issue::intensifies, .name = "intensifies", .reading = "saying it more strongly than its atoms"},
    {.issue = Issue::misattributes,
     .name = "misattributes",
     .reading = "giving a statement to another than who said it"},
    {.issue = Issue::contradicts, .name = "contradicts", .reading = "contradicting its atoms"},
});

constexpr std::string_view kPrompt = "judge";
// A reading's size: a sentence's evidence and what it says beyond it.
constexpr int kTokensPerJudgedSentence = 96;

const IssueName& named(Issue issue) noexcept {
    for (const IssueName& entry : kIssues) {
        if (entry.issue == issue) {
            return entry;
        }
    }
    return kIssues.front();
}

}  // namespace

std::string_view to_string(Issue issue) noexcept {
    return named(issue).name;
}

std::optional<Issue> issue_from_string(std::string_view name) noexcept {
    for (const IssueName& entry : kIssues) {
        if (entry.name == name) {
            return entry.issue;
        }
    }
    return std::nullopt;
}

std::string_view reading_of(Issue issue) noexcept {
    return named(issue).reading;
}

std::vector<std::string> sentences_of(std::string_view draft) {
    const knowledge::ParsedArticle parsed = knowledge::parse_article(draft);
    std::vector<std::string> sentences;
    for (const knowledge::Block& block : parsed.blocks) {
        if (block.kind != knowledge::BlockKind::paragraph) {
            continue;
        }
        for (const knowledge::CitingSentence& sentence : block.sentences) {
            sentences.emplace_back(text::text_of(draft, sentence.span));
        }
    }
    return sentences;
}

std::vector<std::string> judgment_problems(const json::Json& answer, std::size_t count) {
    Problems problems;
    const json::Json& judged = answer.at("sentences");
    std::size_t position = 0;
    for (const json::Json& item : judged) {
        ++position;
        const auto number = item.at("sentence").get<std::size_t>();
        if (number > count) {
            problems.note(std::format("there is no sentence {}: there are {}", number, count));
        } else if (number != position) {
            problems.note(
                std::format("sentence {} is judged where sentence {} should be: judge each sentence "
                            "once, in order",
                            number, position));
        }
    }
    if (judged.size() < count) {
        problems.note(judged.size() + 1 == count
                          ? std::format("sentence {} is not judged: judge every sentence", count)
                          : std::format("sentences {} to {} are not judged: judge every sentence",
                                        judged.size() + 1, count));
    }
    return std::move(problems).list();
}

std::vector<Judged> judgment_of(const json::Json& answer) {
    std::vector<Judged> judged;
    const json::Json& sentences = answer.at("sentences");
    judged.reserve(sentences.size());
    for (const json::Json& item : sentences) {
        const json::Json& reading = item.at("reading");
        judged.push_back(
            Judged{.sentence = item.at("sentence").get<std::size_t>(),
                   .evidence = item.at("evidence").get<std::string>(),
                   .reading = reading.is_null() ? std::nullopt : std::optional{reading.get<std::string>()},
                   .issue = issue_from_string(item.at("issue").get<std::string>()).value_or(Issue::none)});
    }
    return judged;
}

reason::Preference preference_of(const json::Json& answer) {
    const auto better = answer.at("better").get<std::string>();
    if (better == "first") {
        return reason::Preference::first;
    }
    return better == "second" ? reason::Preference::second : reason::Preference::tie;
}

std::size_t issues_in(const std::vector<Judged>& reading) {
    return static_cast<std::size_t>(
        std::ranges::count_if(reading, [](const Judged& sentence) { return sentence.issue != Issue::none; }));
}

Result<agents::Mission> judge_mission(const StageContext& context, std::string_view stage,
                                      const StagePrompt& prompt, const json::Json& shared, ToJudge what) {
    const std::vector<std::string> sentences = sentences_of(what.draft);
    std::string lines;
    for (std::size_t index = 0; index < sentences.size(); ++index) {
        lines += std::format("{}{}. {}", lines.empty() ? "" : "\n", index + 1, sentences[index]);
    }
    const json::Json order{
        {"what", std::move(what.what)}, {"atoms", std::move(what.atoms)}, {"sentences", std::move(lines)}};
    RR_TRY_ASSIGN(const prompt::Rendered rendered, context.prompts->render(kPrompt, shared, order));
    const std::int64_t seed = llm::derived_seed(context.job->salt, stage, what.id, 0);
    const std::int64_t room =
        (static_cast<std::int64_t>(sentences.size()) * kTokensPerJudgedSentence) + kSpareTokens;
    return agents::Mission{
        .id = std::move(what.id),
        .kind = std::string{kPrompt},
        .prompt = prompt.info->label(),
        .role = "infantry",
        .request = llm::ChatRequest{.messages = {llm::Message::system(rendered.system),
                                                 llm::Message::user(rendered.user)},
                                    .seed = seed,
                                    .thinking = false},
        .answer_tokens = static_cast<int>(std::clamp<std::int64_t>(room, kLeastAnswer, kMostAnswer)),
        .schema = prompt.schema,
        .check =
            [count = sentences.size()](const json::Json& answer) { return judgment_problems(answer, count); },
        .escalation = kJudgeEscalation};
}

}  // namespace rr::pipeline
