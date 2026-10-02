#include <rr/oracle/answers.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <format>
#include <memory>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/core/strings.hpp>
#include <rr/json/json.hpp>

namespace rr::oracle {

namespace {

constexpr std::size_t kWordsPerSentence = 12;

std::vector<std::string> words_of(std::string_view text) {
    std::vector<std::string> words;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t begin = text.find_first_not_of(" \t\n\r", at);
        if (begin == std::string_view::npos) {
            break;
        }
        const std::size_t end = std::min(text.find_first_of(" \t\n\r", begin), text.size());
        words.emplace_back(text.substr(begin, end - begin));
        at = end;
    }
    return words;
}

// A word as captions write it: lowercase, without punctuation around it.
// Letters beyond ASCII stay as they are: synthetic scripts are English.
std::string bare(std::string_view word) {
    std::string kept;
    for (const char character : to_lower_ascii(word)) {
        const auto byte = static_cast<unsigned char>(character);
        if ((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte >= 0x80U || byte == '\'') {
            kept += character;
        }
    }
    return kept;
}

std::string joined(const std::vector<std::string>& words, std::size_t begin, std::size_t end) {
    std::string text;
    for (std::size_t index = begin; index < end; ++index) {
        text += index == begin ? words[index] : " " + words[index];
    }
    return text;
}

std::string answer(std::string text) {
    return json::dump(json::Json{{"text", std::move(text)}});
}

// The script's text for `excerpt`: its words where the excerpt's are.
Result<std::string> from_script(const std::vector<std::string>& script,
                                const std::vector<std::string>& bare_script,
                                const std::vector<std::string>& excerpt) {
    std::vector<std::string> wanted;
    wanted.reserve(excerpt.size());
    for (const std::string& word : excerpt) {
        wanted.push_back(bare(word));
    }
    const auto found = std::ranges::search(bare_script, wanted);
    if (found.empty() || wanted.empty()) {
        return fail(Errc::invalid_argument, "the oracle's script does not hold the excerpt's words");
    }
    const auto begin = static_cast<std::size_t>(found.begin() - bare_script.begin());
    return answer(joined(script, begin, begin + wanted.size()));
}

// A sentence of every kWordsPerSentence words: "Word … word. Word …".
std::string every_few(std::vector<std::string> words) {
    for (std::size_t index = 0; index < words.size(); ++index) {
        if (index % kWordsPerSentence == 0) {
            words[index] = to_upper_ascii(words[index].substr(0, 1)) + words[index].substr(1);
        }
        if ((index + 1) % kWordsPerSentence == 0 || index + 1 == words.size()) {
            words[index] += '.';
        }
    }
    return answer(joined(words, 0, words.size()));
}

// What the last message holding "<tag>\n" … "\n</tag>" holds there.
Result<std::string> block_in(const llm::ChatRequest& request, std::string_view tag) {
    const std::string open = std::format("<{}>\n", tag);
    const std::string close = std::format("\n</{}>", tag);
    for (const llm::Message& message : std::views::reverse(request.messages)) {
        const std::string text = message.text();
        const std::size_t at = text.find(open);
        const std::size_t end = at == std::string::npos ? at : text.find(close, at + open.size());
        if (end != std::string::npos) {
            return text.substr(at + open.size(), end - at - open.size());
        }
    }
    return fail(Errc::invalid_argument, std::format("no message of the mission holds <{}>", tag));
}

// A sentence as the annotate prompts show it: "S12 [3:04] What was said."
struct Said {
    std::string id;
    std::string text;
};

std::vector<Said> said_in(std::string_view lines) {
    std::vector<Said> sentences;
    std::size_t at = 0;
    while (at < lines.size()) {
        const std::size_t end = std::min(lines.find('\n', at), lines.size());
        const std::string_view line = lines.substr(at, end - at);
        const std::size_t space = line.find(' ');
        const std::size_t text = line.find("] ");
        if (space != std::string_view::npos && text != std::string_view::npos) {
            sentences.push_back(
                Said{.id = std::string{line.substr(0, space)}, .text = std::string{line.substr(text + 2)}});
        }
        at = end + 1;
    }
    return sentences;
}

json::Json fact(const Said& sentence, std::string_view importance) {
    return json::Json{{"sentences", json::Json::array({sentence.id})},
                      {"quote", sentence.text},
                      {"claim", sentence.text},
                      {"importance", std::string{importance}}};
}

std::string annotation(json::Json facts) {
    return json::dump(json::Json{
        {"facts", std::move(facts)}, {"must_quotes", json::Json::array()}, {"terms", json::Json::array()}});
}

// A word without the punctuation around it: "Ada," is "Ada".
std::string_view trimmed(std::string_view word) {
    const auto letter = [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return std::isalnum(byte) != 0 || byte >= 0x80U;
    };
    while (!word.empty() && !letter(word.front())) {
        word.remove_prefix(1);
    }
    while (!word.empty() && !letter(word.back())) {
        word.remove_suffix(1);
    }
    return word;
}

// Two capitals or more, and nothing else: "NASA", "DNA".
bool acronym(std::string_view word) {
    return word.size() >= 2 && std::ranges::all_of(word, [](char character) {
               return std::isupper(static_cast<unsigned char>(character)) != 0;
           });
}

// The section titles of a brief mission's lines, "1. [0:00] Title: gist":
// what follows "] ", up to ": ".
std::vector<std::string> titles_in(std::string_view lines) {
    std::vector<std::string> titles;
    std::size_t at = 0;
    while (at < lines.size()) {
        const std::size_t end = std::min(lines.find('\n', at), lines.size());
        const std::string_view line = lines.substr(at, end - at);
        if (const std::size_t open = line.find("] "); open != std::string_view::npos) {
            const std::string_view title = line.substr(open + 2);
            titles.emplace_back(title.substr(0, title.find(": ")));
        }
        at = end + 1;
    }
    return titles;
}

}  // namespace

Answerer annotate() {
    return [](const llm::ChatRequest& request) -> Result<std::string> {
        RR_TRY_ASSIGN(const std::string lines, block_in(request, "sentences"));
        json::Json facts = json::Json::array();
        for (const Said& sentence : said_in(lines)) {
            if (words_of(sentence.text).size() >= 5) {
                const bool number = sentence.text.find_first_of("0123456789") != std::string::npos;
                facts.push_back(fact(sentence, number ? "core" : "detail"));
            }
        }
        return annotation(std::move(facts));
    };
}

Answerer annotate_more() {
    return [](const llm::ChatRequest& request) -> Result<std::string> {
        RR_TRY_ASSIGN(const std::string lines, block_in(request, "sentences"));
        RR_TRY_ASSIGN(const std::string found, block_in(request, "found"));
        json::Json facts = json::Json::array();
        for (const Said& sentence : said_in(lines)) {
            // The notes cite sentences as "[S12, S13]": a sentence is noted
            // when its id is there, followed by "]" or ",".
            const bool noted = found.find(sentence.id + "]") != std::string::npos ||
                               found.find(sentence.id + ",") != std::string::npos;
            if (!noted && words_of(sentence.text).size() >= 3) {
                facts.push_back(fact(sentence, "detail"));
            }
        }
        return annotation(std::move(facts));
    };
}

Answerer outline() {
    return [](const llm::ChatRequest& request) -> Result<std::string> {
        RR_TRY_ASSIGN(const std::string lines, block_in(request, "sentences"));
        json::Json sections = json::Json::array();
        for (const Said& sentence : said_in(lines)) {
            if (sentence.id == "S1" || sentence.text.starts_with("Now,") ||
                sentence.text.starts_with("Next,")) {
                std::vector<std::string> words = words_of(sentence.text);
                words.resize(std::min<std::size_t>(5, words.size()));
                words.back() = std::string{trimmed(words.back())};
                sections.push_back(
                    json::Json{{"first", sentence.id}, {"title", joined(words, 0, words.size())}});
            }
        }
        return json::dump(json::Json{{"sections", std::move(sections)}});
    };
}

Answerer describe() {
    return [](const llm::ChatRequest& request) -> Result<std::string> {
        RR_TRY_ASSIGN(const std::string lines, block_in(request, "sentences"));
        const std::vector<Said> sentences = said_in(lines);
        if (sentences.empty()) {
            return fail(Errc::invalid_argument, "the section has no sentences to describe");
        }
        json::Json speakers = json::Json::array();
        json::Json lexicon = json::Json::array();
        std::vector<std::string> listed;
        for (const Said& sentence : sentences) {
            const std::vector<std::string> words = words_of(sentence.text);
            for (std::size_t index = 0; index < words.size(); ++index) {
                const std::string_view word = trimmed(words[index]);
                if (words[index] == "I'm" && index + 1 < words.size()) {
                    speakers.push_back(
                        json::Json{{"name", std::string{trimmed(words[index + 1])}}, {"role", nullptr}});
                } else if (acronym(word) && !std::ranges::contains(listed, word)) {
                    listed.emplace_back(word);
                    lexicon.push_back(json::Json{{"term", std::string{word}}, {"meaning", nullptr}});
                }
            }
        }
        return json::dump(json::Json{{"speakers", std::move(speakers)},
                                     {"lexicon", std::move(lexicon)},
                                     {"gist", sentences.front().text},
                                     {"genre", "talk"}});
    };
}

Answerer brief() {
    return [](const llm::ChatRequest& request) -> Result<std::string> {
        RR_TRY_ASSIGN(const std::string lines, block_in(request, "sections"));
        const std::vector<std::string> titles = titles_in(lines);
        if (titles.empty()) {
            return fail(Errc::invalid_argument, "the video has no sections to sum up");
        }
        return json::dump(json::Json{{"gist", std::format("A video in {} sections, from {} to {}.",
                                                          titles.size(), titles.front(), titles.back())}});
    };
}

Result<std::string> excerpt_in(const llm::ChatRequest& request) {
    // A repair's conversation ends with its instruction: the excerpt is in
    // the order's message, before it.
    return block_in(request, "excerpt");
}

Answerer punctuate(std::optional<std::string> script) {
    if (!script.has_value()) {
        return [](const llm::ChatRequest& request) -> Result<std::string> {
            RR_TRY_ASSIGN(const std::string excerpt, excerpt_in(request));
            return every_few(words_of(excerpt));
        };
    }
    auto words = std::make_shared<const std::vector<std::string>>(words_of(*script));
    auto bare_words = std::make_shared<std::vector<std::string>>();
    for (const std::string& word : *words) {
        bare_words->push_back(bare(word));
    }
    return [words, bare = std::shared_ptr<const std::vector<std::string>>(std::move(bare_words))](
               const llm::ChatRequest& request) -> Result<std::string> {
        RR_TRY_ASSIGN(const std::string excerpt, excerpt_in(request));
        return from_script(*words, *bare, words_of(excerpt));
    };
}

void teach_stages(Oracle& oracle, std::optional<std::string> script) {
    oracle.teach("punctuate", punctuate(std::move(script)));
    oracle.teach("annotate", annotate());
    oracle.teach("annotate_more", annotate_more());
    oracle.teach("outline", outline());
    oracle.teach("describe", describe());
    oracle.teach("brief", brief());
}

}  // namespace rr::oracle
