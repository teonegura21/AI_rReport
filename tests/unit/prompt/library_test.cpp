#include <rr/prompt/library.hpp>

#include <filesystem>
#include <format>
#include <map>
#include <string>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::prompt {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using testkit::unwrap;

constexpr std::string_view kSystem =
    "You are {{ role }}.\n{% for rule in rules %}- {{ rule }} ({{ loop.index1 }})\n{% endfor %}";
constexpr std::string_view kTask = "Sentences:\n{{ packet }}\n\n{{ question }}";
constexpr std::string_view kExample = R"({
  "shared": {"role": "a careful reader", "rules": ["quote exactly", "keep the order"]},
  "orders": [{"packet": "S1 One.", "question": "Who spoke?"},
             {"packet": "S2 Two.", "question": "What was said?"}]
})";

// A library of one prompt, "summarize", whose manifest is right unless
// `edit` changes a file or `versions` the history.
std::map<std::string, std::string> files_with(const std::string& versions) {
    return {{"prompts/manifest.toml", std::format(R"toml(
[[prompt]]
name = "summarize"
purpose = "Say what a window of sentences says."
shared = ["role", "rules"]
failure_modes = ["drops the last sentence"]
{}
)toml",
                                                  versions)},
            {"prompts/summarize/system.md.inja", std::string{kSystem}},
            {"prompts/summarize/task.md.inja", std::string{kTask}},
            {"prompts/summarize/example.json", std::string{kExample}}};
}

std::string version(int number, std::string_view hash) {
    return std::format("  [[prompt.version]]\n  number = {}\n  hash = \"{}\"\n  note = \"v{}\"\n", number,
                       hash, number);
}

// The hash of the files as they are.
std::string hash_of(std::map<std::string, std::string> files) {
    return unwrap(unwrap(Library::from_files(std::move(files))).content_hash("summarize"));
}

Library summarize() {
    const std::string hash = hash_of(files_with(version(1, "placeholder")));
    return unwrap(Library::from_files(files_with(version(1, hash))));
}

json::Json shared() {
    return json::Json{{"role", "a careful reader"}, {"rules", {"quote exactly", "keep the order"}}};
}

TEST(PromptLibrary, ReadsTheManifest) {
    const Library library = summarize();
    ASSERT_EQ(library.prompts().size(), 1U);
    const PromptInfo& info = library.prompts()[0];
    EXPECT_EQ(info.name, "summarize");
    EXPECT_EQ(info.shared, (std::vector<std::string>{"role", "rules"}));
    EXPECT_EQ(info.label(), "summarize@1");
    EXPECT_EQ(library.find("nothing"), nullptr);
    EXPECT_THAT(library.problems(), IsEmpty());
}

TEST(PromptLibrary, TheSystemMessageIsTheSameForEveryOrder) {
    const Library library = summarize();
    const Rendered one =
        unwrap(library.render("summarize", shared(), {{"packet", "S1 One."}, {"question", "Who spoke?"}}));
    const Rendered two =
        unwrap(library.render("summarize", shared(), {{"packet", "S9 Nine."}, {"question", "Why?"}}));
    EXPECT_EQ(one.system, "You are a careful reader.\n- quote exactly (1)\n- keep the order (2)\n");
    EXPECT_EQ(one.system, two.system) << "rendered from the shared data alone: a cache can serve it";
    EXPECT_EQ(one.user, "Sentences:\nS1 One.\n\nWho spoke?");
    EXPECT_NE(one.user, two.user);
}

TEST(PromptLibrary, RenderingIsStrict) {
    const Library library = summarize();
    const auto missing = library.render("summarize", shared(), {{"question", "Who spoke?"}});
    ASSERT_ERRC(missing, Errc::invalid_argument);
    EXPECT_THAT(missing.error().message(), HasSubstr("packet")) << "never an empty packet, silently";

    EXPECT_ERRC(library.render("summarize", shared(),
                               {{"role", "someone else"}, {"packet", "S1"}, {"question", "?"}}),
                Errc::invalid_argument)
        << "a key both shared and the order's is ambiguous";
    EXPECT_ERRC(library.render("nothing", shared(), json::Json::object()), Errc::not_found);

    // The system part is rendered with the shared data alone: a slip there
    // fails, even though the order has what it names.
    std::map<std::string, std::string> files = files_with(version(1, "placeholder"));
    files["prompts/summarize/system.md.inja"] = "You are {{ role }}. Read {{ packet }}.";
    const auto leaked = unwrap(Library::from_files(std::move(files)))
                            .render("summarize", shared(), {{"packet", "S1 One."}, {"question", "?"}});
    ASSERT_ERRC(leaked, Errc::invalid_argument);
    EXPECT_THAT(leaked.error().message(), HasSubstr("packet"));
    EXPECT_ERRC(library.render("summarize", json::Json::array(), json::Json::object()),
                Errc::invalid_argument);
}

TEST(PromptLibrary, TheSystemPartMayUseOnlySharedVariables) {
    for (const std::string_view system : {
             std::string_view{"You are {{ role }}. Read {{ packet }}."},
             // In a branch no example takes: the check reads the template, not a rendering.
             std::string_view{"You are {{ role }}.{% if false %}{{ packet.first }}{% endif %}"},
             std::string_view{"{% for item in packet %}{{ item }}{% endfor %}"},
             std::string_view{"{% if role %}{{ role }}{% else %}{{ packet }}{% endif %}"},
         }) {
        std::map<std::string, std::string> files = files_with(version(1, "placeholder"));
        files["prompts/summarize/system.md.inja"] = std::string{system};
        const std::string hash = hash_of(files);
        files = files_with(version(1, hash));
        files["prompts/summarize/system.md.inja"] = std::string{system};
        const auto problems = unwrap(Library::from_files(std::move(files))).problems();
        EXPECT_THAT(problems, Contains(HasSubstr("its system part uses 'packet', which is not shared")))
            << system;
    }
}

TEST(PromptLibrary, AChangedPromptNeedsANewVersion) {
    const std::string first = hash_of(files_with(version(1, "placeholder")));
    std::map<std::string, std::string> changed = files_with(version(1, first));
    changed["prompts/summarize/task.md.inja"] = "Sentences:\n{{ packet }}\n\nAnswer: {{ question }}";
    const std::string second = hash_of(changed);
    ASSERT_NE(first, second);
    EXPECT_THAT(
        unwrap(Library::from_files(changed)).problems(),
        Contains(std::format("prompt 'summarize' changed since version 1: add [[prompt.version]] number "
                             "= 2, hash = \"{}\", with a note on what changed",
                             second)))
        << "the line says exactly what to add";

    // Text moved from one part to the other changes the prompt: what the
    // cache can share has changed. The two parts concatenate alike, so the
    // hash frames each file with its path and length.
    std::map<std::string, std::string> moved = files_with(version(1, first));
    moved["prompts/summarize/system.md.inja"] += "Sentences:\n";
    moved["prompts/summarize/task.md.inja"] = "{{ packet }}\n\n{{ question }}";
    EXPECT_THAT(unwrap(Library::from_files(moved)).problems(),
                Contains(HasSubstr("prompt 'summarize' changed since version 1")));

    std::map<std::string, std::string> bumped = files_with(version(1, first) + version(2, second));
    bumped["prompts/summarize/task.md.inja"] = changed["prompts/summarize/task.md.inja"];
    const Library library = unwrap(Library::from_files(std::move(bumped)));
    EXPECT_THAT(library.problems(), IsEmpty());
    EXPECT_EQ(library.find("summarize")->label(), "summarize@2");
}

TEST(PromptLibrary, VersionsCountFromOneAndNeverRepeatAHash) {
    const std::string hash = hash_of(files_with(version(1, "placeholder")));
    EXPECT_THAT(unwrap(Library::from_files(files_with(version(2, hash)))).problems(),
                Contains(HasSubstr("version #1 is numbered 2, expected 1")));
    EXPECT_THAT(unwrap(Library::from_files(files_with(version(1, hash) + version(2, hash)))).problems(),
                Contains(HasSubstr("two versions have the hash")));
}

TEST(PromptLibrary, ASchemaIsPartOfThePrompt) {
    std::map<std::string, std::string> files = files_with(version(1, "placeholder"));
    files["prompts/manifest.toml"].insert(files["prompts/manifest.toml"].find("shared"),
                                          "schema = \"schemas/missions/summary.json\"\n");
    files["schemas/missions/summary.json"] =
        R"({"type":"object","properties":{"summary":{"type":"string"}},"required":["summary"],"additionalProperties":false})";
    const std::string hash = hash_of(files);
    std::map<std::string, std::string> versioned = files;
    versioned["prompts/manifest.toml"].replace(versioned["prompts/manifest.toml"].find("placeholder"), 11,
                                               hash);
    EXPECT_THAT(unwrap(Library::from_files(versioned)).problems(), IsEmpty());

    versioned["schemas/missions/summary.json"] = R"({"type":"object","properties":{}})";
    EXPECT_THAT(unwrap(Library::from_files(versioned)).problems(),
                Contains(HasSubstr("prompt 'summarize' changed since version 1")))
        << "a new schema changes what the model is asked";
    versioned["schemas/missions/summary.json"] = "{not json";
    EXPECT_THAT(unwrap(Library::from_files(versioned)).problems(), Contains(HasSubstr("does not compile")));
    versioned.erase("schemas/missions/summary.json");
    EXPECT_THAT(unwrap(Library::from_files(versioned)).problems(), Contains(HasSubstr("is missing")));
}

TEST(PromptLibrary, EveryPromptHasItsFilesAndAnExampleThatRenders) {
    std::map<std::string, std::string> files = files_with(version(1, "placeholder"));
    files["prompts/summarize/example.json"] = "{}";
    EXPECT_THAT(unwrap(Library::from_files(files)).problems(),
                Contains(HasSubstr("must be {\"shared\": {…}, \"orders\": [two or more]}")));

    files = files_with(version(1, "placeholder"));
    files["prompts/summarize/example.json"] =
        R"({"shared": {"role": "r", "rules": []}, "orders": [{"packet": "S1"}, {"packet": "S2"}]})";
    EXPECT_THAT(unwrap(Library::from_files(files)).problems(),
                Contains(HasSubstr("its example does not render")))
        << "the example lacks `question`";

    files = files_with(version(1, "placeholder"));
    files["prompts/orphan/system.md.inja"] = "Hello.";
    EXPECT_THAT(unwrap(Library::from_files(files)).problems(),
                Contains("prompts/orphan/ has no [[prompt]] in prompts/manifest.toml"));
}

TEST(PromptLibrary, TemplatesReadNothingFromTheFilesystem) {
    std::map<std::string, std::string> files = files_with(version(1, "placeholder"));
    files["prompts/summarize/task.md.inja"] = "{% include \"/etc/hostname\" %}{{ packet }}{{ question }}";
    EXPECT_ERRC(unwrap(Library::from_files(std::move(files)))
                    .render("summarize", shared(), {{"packet", "S1"}, {"question", "?"}}),
                Errc::invalid_argument)
        << "a prompt is its files in the library, and nothing else on the machine";
}

TEST(PromptLibrary, ManifestMistakesAreRefusedWhenLoaded) {
    const auto refused = [](const std::string& versions) {
        return Library::from_files(files_with(versions));
    };
    EXPECT_ERRC(refused(""), Errc::invalid_argument) << "a prompt has at least one version";
    EXPECT_ERRC(refused("  [[prompt.version]]\n  number = \"1\"\n  hash = \"x\"\n"), Errc::invalid_argument);
    std::map<std::string, std::string> twice = files_with(version(1, "x"));
    twice["prompts/manifest.toml"] += twice["prompts/manifest.toml"];
    EXPECT_ERRC(Library::from_files(twice), Errc::invalid_argument);
}

TEST(PromptLibrary, ReadsAProjectDirectory) {
    const testkit::TempDir dir{"rr_prompt_library"};
    const std::string hash = hash_of(files_with(version(1, "placeholder")));
    for (const auto& [path, content] : files_with(version(1, hash))) {
        (void)dir.write_file(path, content);
    }
    const Library library = unwrap(Library::from_directory(dir.path()));
    EXPECT_THAT(library.problems(), IsEmpty());
    EXPECT_EQ(unwrap(library.content_hash("summarize")), hash);
}

// The L0 check: the prompts this binary carries, and the ones in the
// source tree, are the same, and have no problem.
TEST(PromptLibrary, TheShippedPromptsHaveNoProblems) {
    const Library shipped = unwrap(Library::embedded());
    EXPECT_THAT(shipped.problems(), IsEmpty());
    const Library sources = unwrap(Library::from_directory(RR_SOURCE_DIR));
    EXPECT_EQ(shipped.prompts(), sources.prompts()) << "rebuild: the embedded prompts are stale";
}

}  // namespace
}  // namespace rr::prompt
