#include <rr/config/config.hpp>

#include <filesystem>

#include <gtest/gtest.h>

#include <rr/testkit/temp_dir.hpp>

namespace rr::config {
namespace {

using std::chrono::milliseconds;

constexpr std::string_view kDefaults = R"toml(
[runtime]
url = ""
token = ""
timeout = "30s"
slots = 4
temperature = 0.2
verify_tls = true

[paths]
library = "~/rreport/library"

[capture]
languages = ["en"]
)toml";

LoadOptions base_options() {
    LoadOptions options;
    options.defaults_toml = kDefaults;
    options.environment = std::vector<std::pair<std::string, std::string>>{};  // hermetic
    return options;
}

TEST(Config, DefaultsProvideTypedValues) {
    const Config config = Config::load(base_options()).value();
    EXPECT_EQ(config.get_string("runtime.url").value(), "");
    EXPECT_EQ(config.get_int("runtime.slots").value(), 4);
    EXPECT_DOUBLE_EQ(config.get_double("runtime.temperature").value(), 0.2);
    EXPECT_TRUE(config.get_bool("runtime.verify_tls").value());
    EXPECT_EQ(config.get_duration("runtime.timeout").value(), milliseconds{30'000});
    EXPECT_EQ(config.get_string_list("capture.languages").value(), (std::vector<std::string>{"en"}));
    EXPECT_EQ(config.origin("runtime.slots")->layer, Layer::defaults);
}

TEST(Config, LayersOverrideInOrder_FileThenEnvThenCommandLine) {
    const testkit::TempDir dir{"rr_config_test"};
    LoadOptions options = base_options();
    options.file = dir.write_file("config.toml", "[runtime]\nurl = \"https://file\"\nslots = 8\n");
    options.environment = std::vector<std::pair<std::string, std::string>>{
        {"RREPORT_RUNTIME__SLOTS", "16"},
        {"RREPORT_CAPTURE__LANGUAGES", "en, ro"},
        {"UNRELATED", "x"},
        {"RREPORT_CONFIG", "/ignored/app/level/variable"},
    };
    options.overrides = {"runtime.slots=32"};

    const Config config = Config::load(options).value();
    EXPECT_EQ(config.get_string("runtime.url").value(), "https://file");
    EXPECT_EQ(config.origin("runtime.url")->layer, Layer::file);
    EXPECT_EQ(config.get_int("runtime.slots").value(), 32);
    EXPECT_EQ(config.origin("runtime.slots")->layer, Layer::command_line);
    EXPECT_EQ(config.get_string_list("capture.languages").value(), (std::vector<std::string>{"en", "ro"}));
    EXPECT_EQ(config.origin("capture.languages")->detail, "RREPORT_CAPTURE__LANGUAGES");
}

TEST(Config, UnknownKeyInFile_FailsWithSuggestion) {
    const testkit::TempDir dir{"rr_config_test"};
    LoadOptions options = base_options();
    options.file = dir.write_file("config.toml", "[runtime]\nulr = \"https://typo\"\n");
    const auto result = Config::load(options);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message().find("did you mean 'runtime.url'"), std::string::npos)
        << result.error().message();
}

TEST(Config, TypeMismatch_NamesKeyAndSource) {
    const testkit::TempDir dir{"rr_config_test"};
    LoadOptions options = base_options();
    const auto file = dir.write_file("config.toml", "[runtime]\nslots = \"many\"\n");
    options.file = file;
    const auto result = Config::load(options);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message().find("runtime.slots"), std::string::npos) << result.error().message();
    EXPECT_NE(result.error().message().find("must be an integer"), std::string::npos)
        << result.error().message();
}

TEST(Config, IntegerAcceptedWhereNumberExpected) {
    LoadOptions options = base_options();
    options.overrides = {"runtime.temperature=1"};
    EXPECT_DOUBLE_EQ(Config::load(options).value().get_double("runtime.temperature").value(), 1.0);
}

TEST(Config, BadTextualValues_AreRejected) {
    for (const char* bad : {"runtime.slots=4x", "runtime.verify_tls=maybe", "runtime.temperature=warm",
                            "noequals", "=1", "runtime.nope=1"}) {
        LoadOptions options = base_options();
        options.overrides = {bad};
        EXPECT_FALSE(Config::load(options).has_value()) << bad;
    }
}

TEST(Config, BooleanSpellings) {
    for (const auto& [text, expected] : std::vector<std::pair<std::string, bool>>{{"true", true},
                                                                                  {"YES", true},
                                                                                  {"on", true},
                                                                                  {"1", true},
                                                                                  {"false", false},
                                                                                  {"Off", false},
                                                                                  {"0", false}}) {
        LoadOptions options = base_options();
        options.overrides = {"runtime.verify_tls=" + text};
        EXPECT_EQ(Config::load(options).value().get_bool("runtime.verify_tls").value(), expected) << text;
    }
}

TEST(Config, MissingOptionalFileIsSkipped_MissingRequiredFileFails) {
    LoadOptions options = base_options();
    options.file = "/definitely/missing/config.toml";
    EXPECT_TRUE(Config::load(options).has_value());
    options.file_required = true;
    EXPECT_EQ(Config::load(options).error().code(), Errc::not_found);
}

TEST(Config, TomlSyntaxErrors_IncludeLineAndColumn) {
    const testkit::TempDir dir{"rr_config_test"};
    LoadOptions options = base_options();
    options.file = dir.write_file("config.toml", "[runtime\nurl = 1\n");
    const auto result = Config::load(options);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message().find("config.toml:1:"), std::string::npos) << result.error().message();
}

TEST(Config, SecretsAreMaskedAndPermissionsEnforced) {
    const testkit::TempDir dir{"rr_config_test"};
    LoadOptions options = base_options();
    const auto secrets = dir.write_file("secrets.toml", "[runtime]\ntoken = \"tk-123\"\n");
    options.secrets_file = secrets;

    std::filesystem::permissions(secrets, std::filesystem::perms::owner_read |
                                              std::filesystem::perms::owner_write |
                                              std::filesystem::perms::group_read);
    const auto exposed = Config::load(options);
    ASSERT_FALSE(exposed.has_value());
    EXPECT_EQ(exposed.error().code(), Errc::permission_denied);
    EXPECT_NE(exposed.error().message().find("chmod 600"), std::string::npos);

    std::filesystem::permissions(secrets,
                                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    const Config config = Config::load(options).value();
    EXPECT_EQ(config.get_secret("runtime.token").value().expose(), "tk-123");
    const std::string listing = config.show(/*with_origins=*/true);
    EXPECT_EQ(listing.find("tk-123"), std::string::npos) << listing;
    EXPECT_NE(listing.find("runtime.token = \"***\"  # secrets_file"), std::string::npos) << listing;
    EXPECT_NE(listing.find("runtime.url = \"\"  # defaults"), std::string::npos) << listing;
}

TEST(Config, PathsExpandHome) {
    const Config config = Config::load(base_options()).value();
    const auto path = config.get_path("paths.library").value();
    EXPECT_TRUE(path.is_absolute()) << path;
    EXPECT_TRUE(path.string().ends_with("rreport/library")) << path;
}

TEST(Config, WrongTypeAccessAndUnknownKeys) {
    const Config config = Config::load(base_options()).value();
    EXPECT_EQ(config.get_int("runtime.url").error().code(), Errc::invalid_argument);
    EXPECT_EQ(config.get_int("runtime.missing").error().code(), Errc::not_found);
    EXPECT_FALSE(config.contains("runtime.missing"));
    EXPECT_FALSE(config.origin("runtime.missing").has_value());
}

TEST(SecretKeys, RecognizedByLastSegment) {
    EXPECT_TRUE(is_secret_key("runtime.token"));
    EXPECT_TRUE(is_secret_key("providers.claude.api_key"));
    EXPECT_TRUE(is_secret_key("pod.runpod_api_key"));
    EXPECT_TRUE(is_secret_key("db.password"));
    EXPECT_FALSE(is_secret_key("runtime.url"));
    EXPECT_FALSE(is_secret_key("tokenizer.path"));
}

TEST(Durations, ParseUnits) {
    EXPECT_EQ(parse_duration("250ms").value(), milliseconds{250});
    EXPECT_EQ(parse_duration("30s").value(), milliseconds{30'000});
    EXPECT_EQ(parse_duration("5m").value(), milliseconds{300'000});
    EXPECT_EQ(parse_duration("2h").value(), milliseconds{7'200'000});
    for (const char* bad : {"", "30", "s", "1.5s", "-1s", "10d"}) {
        EXPECT_FALSE(parse_duration(bad).has_value()) << bad;
    }
}

}  // namespace
}  // namespace rr::config
