#include <rr/oracle/answers.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <rr/core/strings.hpp>
#include <rr/json/json.hpp>

namespace rr::oracle {

namespace {

constexpr std::size_t kWordsPerSentence = 12;
constexpr std::size_t kPlannedAtoms = 3;  // a paragraph's, in a plan

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

// --- What a sentence says (atomize()) -------------------------------------------

// The words of a sentence of filler; and those of a greeting, which opens
// with one of kGreetings.
constexpr std::array<std::string_view, 10> kFillerWords{"um",   "uh",   "er",    "hmm",  "so",
                                                        "well", "okay", "right", "like", "yeah"};
constexpr std::array<std::string_view, 6> kGreetings{"hello", "hi", "hey", "welcome", "thanks", "thank"};
constexpr std::array<std::string_view, 14> kGreetingWords{"you", "everyone", "everybody", "all",     "back",
                                                          "and", "to",       "the",       "channel", "show",
                                                          "for", "watching", "much",      "again"};

// The filler label atomize() gives `text`, or none.
std::optional<std::string> filler_label(std::string_view text) {
    const std::vector<std::string> words = words_of(text);
    bool filler = !words.empty();    // filler words alone
    bool greeting = !words.empty();  // greeting and filler words alone
    bool greets = false;             // one greeting word at least
    for (const std::string& word : words) {
        const std::string plain = bare(word);
        const bool filler_word = std::ranges::contains(kFillerWords, plain);
        const bool greeting_word = std::ranges::contains(kGreetings, plain);
        filler = filler && filler_word;
        greeting = greeting && (filler_word || greeting_word || std::ranges::contains(kGreetingWords, plain));
        greets = greets || greeting_word;
    }
    if (filler) {
        return "disfluency";
    }
    if (greeting && greets) {
        return "greeting";
    }
    return std::nullopt;
}

// Whether `text` ends a sentence: a full stop, a question or an exclamation
// mark, or an ellipsis, before any closing quotes or brackets.
bool ends_a_sentence(std::string_view text) {
    while (!text.empty() && std::string_view{"\"')] "}.contains(text.back())) {
        text.remove_suffix(1);
    }
    return text.ends_with('.') || text.ends_with('?') || text.ends_with('!') || text.ends_with("\u2026");
}

// 12 for "S12"; 0 for anything else.
std::uint32_t number_of(std::string_view id) {
    std::uint32_t number = 0;
    const bool parsed = id.size() >= 2 && id.front() == 'S' &&
                        std::from_chars(id.data() + 1, id.data() + id.size(), number).ec == std::errc{};
    return parsed ? number : 0;
}

// The runs of capitalized words after the first word of `text`, each a name
// ("Ada Lovelace"), once each. A word with punctuation after it ends its run.
std::vector<std::string> names_in(std::string_view text) {
    const std::vector<std::string> words = words_of(text);
    std::vector<std::string> names;
    std::string run;
    const auto close = [&] {
        if (!run.empty() && !std::ranges::contains(names, run)) {
            names.push_back(run);
        }
        run.clear();
    };
    for (std::size_t index = 1; index < words.size(); ++index) {
        const std::string_view word = trimmed(words[index]);
        const bool capitalized = word.size() >= 2 && std::isupper(static_cast<unsigned char>(word[0])) != 0 &&
                                 std::islower(static_cast<unsigned char>(word[1])) != 0;
        if (!capitalized) {
            close();
            continue;
        }
        run += run.empty() ? std::string{word} : " " + std::string{word};
        if (!words[index].ends_with(word)) {
            close();
        }
    }
    close();
    return names;
}

// The acronyms of `text`, once each.
std::vector<std::string> acronyms_in(std::string_view text) {
    std::vector<std::string> terms;
    for (const std::string& word : words_of(text)) {
        if (const std::string_view plain = trimmed(word);
            acronym(plain) && !std::ranges::contains(terms, plain)) {
            terms.emplace_back(plain);
        }
    }
    return terms;
}

// The words between the first pair of double quotes of `text`, or none.
std::optional<std::string> quoted_in(std::string_view text) {
    const std::size_t open = text.find('"');
    const std::size_t close = open == std::string_view::npos ? open : text.find('"', open + 1);
    if (close == std::string_view::npos || close == open + 1) {
        return std::nullopt;
    }
    return std::string{text.substr(open + 1, close - open - 1)};
}

// The atom atomize() writes of `text`, which also draws on `also`.
json::Json atom_of(const std::string& text, json::Json also) {
    const bool number = text.find_first_of("0123456789") != std::string::npos;
    const bool question = text.ends_with('?');
    const bool hedged = text.find("maybe") != std::string::npos || text.find("Maybe") != std::string::npos ||
                        text.find("I think") != std::string::npos;
    std::string kind = "claim";
    if (number) {
        kind = "number";
    } else if (question) {
        kind = "question";
    }
    std::string modality = "asserted";
    if (question) {
        modality = "question";
    } else if (hedged) {
        modality = "hedged";
    }
    const std::optional<std::string> quote = quoted_in(text);
    return json::Json{{"also", std::move(also)},
                      {"names", names_in(text)},
                      {"terms", acronyms_in(text)},
                      {"text", text},
                      {"kind", std::move(kind)},
                      {"modality", std::move(modality)},
                      {"register", "main"},
                      {"intensity", "medium"},
                      {"speaker", nullptr},
                      {"attributed_to", nullptr},
                      {"must_quote", quote.has_value() ? json::Json(*quote) : json::Json(nullptr)}};
}

json::Json row_of(const Said& sentence, json::Json filler, json::Json atoms) {
    return json::Json{{"sentence", sentence.id}, {"filler", std::move(filler)}, {"atoms", std::move(atoms)}};
}

// The title of the section a plan mission asks about: in 'Plan section 2,
// "The samples", from 5:00 to 9:55', what stands between the quotes.
Result<std::string> section_in(const llm::ChatRequest& request) {
    constexpr std::string_view kOpen = "Plan section ";
    constexpr std::string_view kClose = "\", from ";
    for (const llm::Message& message : std::views::reverse(request.messages)) {
        const std::string text = message.text();
        const std::size_t at = text.find(kOpen);
        const std::size_t begin = at == std::string::npos ? at : text.find('"', at);
        const std::size_t end = begin == std::string::npos ? begin : text.find(kClose, begin);
        if (end != std::string::npos) {
            return text.substr(begin + 1, end - begin - 1);
        }
    }
    return fail(Errc::invalid_argument, "no message of the mission names the section to plan");
}

// The words of an atom as the write prompt shows it, after its labels and
// before what must reach its paragraph: "A7 [1:02] number: The bridge failed
// at four tons. Names: …" is "The bridge failed at four tons.".
std::string_view atom_words(std::string_view shown) {
    std::size_t end = shown.size();
    for (const std::string_view after : {" To quote: ", " Names: ", " Terms: "}) {
        end = std::min(end, shown.find(after));
    }
    return shown.substr(0, end);
}

// The words to quote of an atom as the write prompt shows it, or none.
std::optional<std::string> to_quote(std::string_view shown) {
    constexpr std::string_view kOpen = " To quote: \"";
    const std::size_t at = shown.find(kOpen);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t begin = at + kOpen.size();
    const std::size_t end = shown.find("\".", begin);
    return std::string{shown.substr(begin, end - begin)};
}

// The atoms a sentence cites, as written: "A7" for "[A7]".
std::vector<std::string> cited_in(std::string_view sentence) {
    std::vector<std::string> atoms;
    for (std::size_t at = sentence.find("[A"); at != std::string_view::npos;
         at = sentence.find("[A", at + 1)) {
        const std::size_t end = sentence.find(']', at);
        if (end == std::string_view::npos) {
            break;
        }
        atoms.emplace_back(sentence.substr(at + 1, end - at - 1));
    }
    return atoms;
}

// "Tells A12.", "Tells A12 and A13.", "Tells A12 to A14.".
std::string purpose_of(const std::vector<std::string>& atoms) {
    if (atoms.size() == 1) {
        return std::format("Tells {}.", atoms.front());
    }
    return std::format("Tells {} {} {}.", atoms.front(), atoms.size() == 2 ? "and" : "to", atoms.back());
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

Answerer atomize() {
    return [](const llm::ChatRequest& request) -> Result<std::string> {
        RR_TRY_ASSIGN(const std::string lines, block_in(request, "sentences"));
        // Asked again what the sentences' atoms miss, the request shows them.
        const Result<std::string> held = block_in(request, "atoms");
        const std::vector<Said> sentences = said_in(lines);
        json::Json rows = json::Json::array();
        for (std::size_t index = 0; index < sentences.size(); ++index) {
            const Said& sentence = sentences[index];
            if (const std::optional<std::string> label = filler_label(sentence.text);
                label.has_value() && !held.has_value()) {
                rows.push_back(row_of(sentence, *label, json::Json::array()));
                continue;
            }
            const bool runs_on = !ends_a_sentence(sentence.text) && index + 1 < sentences.size() &&
                                 number_of(sentences[index + 1].id) == number_of(sentence.id) + 1;
            std::string text = sentence.text;
            json::Json also = json::Json::array();
            if (runs_on) {
                text += " " + sentences[index + 1].text;
                also.push_back(sentences[index + 1].id);
            }
            json::Json atoms = json::Json::array();
            if (!held.has_value() || held->find(text) == std::string::npos) {
                atoms.push_back(atom_of(text, std::move(also)));
            }
            rows.push_back(row_of(sentence, nullptr, std::move(atoms)));
            if (runs_on) {
                rows.push_back(row_of(sentences[++index], nullptr, json::Json::array()));
            }
        }
        return json::dump(json::Json{{"sentences", std::move(rows)}});
    };
}

Answerer plan() {
    return [](const llm::ChatRequest& request) -> Result<std::string> {
        RR_TRY_ASSIGN(const std::string lines, block_in(request, "atoms"));
        RR_TRY_ASSIGN(std::string heading, section_in(request));
        json::Json paragraphs = json::Json::array();
        json::Json duplicates = json::Json::array();
        std::vector<Said> told;  // the atoms a paragraph tells, as shown
        std::vector<std::string> paragraph;
        const auto close = [&] {
            if (!paragraph.empty()) {
                json::Json atoms = json::Json::array();
                for (const std::string& id : paragraph) {
                    atoms.push_back(id);
                }
                paragraphs.push_back(
                    json::Json{{"purpose", purpose_of(paragraph)}, {"atoms", std::move(atoms)}});
                paragraph.clear();
            }
        };
        for (const Said& atom : said_in(lines)) {
            const auto earlier =
                std::ranges::find_if(told, [&](const Said& shown) { return shown.text == atom.text; });
            if (earlier != told.end()) {
                duplicates.push_back(
                    json::Json{{"because", std::format("It says again what {} says.", earlier->id)},
                               {"atom", atom.id},
                               {"of", earlier->id}});
                continue;
            }
            told.push_back(atom);
            paragraph.push_back(atom.id);
            if (paragraph.size() == kPlannedAtoms) {
                close();
            }
        }
        close();
        return json::dump(json::Json{{"paragraphs", std::move(paragraphs)},
                                     {"duplicates", std::move(duplicates)},
                                     {"heading", std::move(heading)}});
    };
}

Answerer write() {
    return [](const llm::ChatRequest& request) -> Result<std::string> {
        RR_TRY_ASSIGN(const std::string lines, block_in(request, "paragraphs"));
        std::vector<std::string> paragraphs;
        for (const auto part : std::views::split(lines, '\n')) {
            const std::string_view line{part.begin(), part.end()};
            if (line.starts_with("Paragraph ")) {
                paragraphs.emplace_back();
                continue;
            }
            const std::size_t time = line.find("] ");
            const std::size_t words = time == std::string_view::npos ? time : line.find(": ", time);
            if (paragraphs.empty() || words == std::string_view::npos) {
                return fail(Errc::invalid_argument, std::format("no paragraph to write \"{}\" in", line));
            }
            const std::string_view shown = line.substr(words + 2);
            std::string sentence{atom_words(shown)};
            if (const std::optional<std::string> quote = to_quote(shown);
                quote.has_value() && sentence.find(*quote) == std::string::npos) {
                sentence += std::format(" (\"{}\")", *quote);
            }
            paragraphs.back() += std::format("{}{} [{}]", paragraphs.back().empty() ? "" : " ", sentence,
                                             line.substr(0, line.find(' ')));
        }
        if (paragraphs.empty()) {
            return fail(Errc::invalid_argument, "no paragraph to write");
        }
        std::string draft;
        for (const std::string& paragraph : paragraphs) {
            draft += (draft.empty() ? "" : "\n\n") + paragraph;
        }
        return draft;
    };
}

Answerer judge() {
    return [](const llm::ChatRequest& request) -> Result<std::string> {
        RR_TRY_ASSIGN(const std::string atoms, block_in(request, "atoms"));
        RR_TRY_ASSIGN(const std::string sentences, block_in(request, "sentences"));
        std::map<std::string, std::string, std::less<>> words;  // "A7": its words
        for (const auto part : std::views::split(atoms, '\n')) {
            const std::string_view line{part.begin(), part.end()};
            const std::size_t time = line.find("] ");
            const std::size_t said = time == std::string_view::npos ? time : line.find(": ", time);
            if (said == std::string_view::npos) {
                return fail(Errc::invalid_argument, std::format("no atom in \"{}\"", line));
            }
            words.emplace(line.substr(0, line.find(' ')), atom_words(line.substr(said + 2)));
        }
        json::Json judged = json::Json::array();
        for (const auto part : std::views::split(sentences, '\n')) {
            const std::string_view line{part.begin(), part.end()};
            const std::size_t dot = line.find(". ");
            const std::string_view digits = line.substr(0, dot);  // the whole line, without a dot
            std::size_t number = 0;
            const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
            if (dot == std::string_view::npos || error != std::errc{} ||
                end != digits.data() + digits.size()) {
                return fail(Errc::invalid_argument, std::format("no numbered sentence in \"{}\"", line));
            }
            std::string evidence;
            for (const std::string& atom : cited_in(line.substr(dot + 2))) {
                const auto found = words.find(atom);
                evidence += std::format("{}{}: \"{}\"", evidence.empty() ? "" : "; ", atom,
                                        found == words.end() ? std::string{"?"} : found->second);
            }
            judged.push_back(json::Json{
                {"sentence", number}, {"evidence", evidence}, {"reading", nullptr}, {"issue", "none"}});
        }
        return json::dump(json::Json{{"sentences", std::move(judged)}});
    };
}

Answerer flow() {
    return [](const llm::ChatRequest& request) -> Result<std::string> {
        RR_TRY_ASSIGN(const std::string first, block_in(request, "first"));
        RR_TRY_ASSIGN(const std::string second, block_in(request, "second"));
        std::string_view better = "tie";
        if (first.size() < second.size()) {
            better = "first";
        } else if (second.size() < first.size()) {
            better = "second";
        }
        return json::dump(json::Json{{"reasons", "The shorter says it in fewer words."}, {"better", better}});
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
    oracle.teach("atomize", atomize());
    oracle.teach("plan", plan());
    oracle.teach("write", write());
    oracle.teach("judge", judge());
    oracle.teach("flow", flow());
}

}  // namespace rr::oracle
