#include <rr/supervisor/catalog.hpp>

#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::supervisor {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using testkit::unwrap;

const std::string kSha(64, 'a');

std::string catalog_text() {
    return std::format(R"toml(
[[model]]
id = "commander"
role = "commander"
file = "qwen/Qwen3.8-27B-Q4_K_M.gguf"
sha256 = "{0}"
url = "https://example.invalid/qwen.gguf"

[[model]]
id = "infantry"
role = "infantry"
file = "gemma/gemma-4-26b-a4b-Q4_K_M.gguf"
sha256 = "{0}"
projector = {{ file = "gemma/mmproj.gguf", sha256 = "{0}" }}

[[model]]
id = "embedder"
role = "embedder"
file = "embed/qwen3-embedding-0.6b.gguf"
sha256 = "{0}"

[[profile]]
name = "24gb"
min_vram_mib = 22000
  [[profile.engine]]
  model = "commander"
  slots = 2
  context = 16384

[[profile]]
name = "48gb"
min_vram_mib = 44000
  [[profile.engine]]
  model = "commander"
  slots = 4
  context = 32768
  extra_args = ["--flash-attn", "on"]
  [[profile.engine]]
  model = "infantry"
  slots = 16
  context = 65536
  [[profile.engine]]
  model = "embedder"
)toml",
                       kSha);
}

TEST(Catalog, ParsesModelsAndProfiles) {
    const Catalog catalog = unwrap(parse_catalog(catalog_text()));
    ASSERT_EQ(catalog.models.size(), 3U);
    const Model* infantry = catalog.find_model("infantry");
    ASSERT_NE(infantry, nullptr);
    ASSERT_TRUE(infantry->projector.has_value());
    EXPECT_EQ(infantry->projector->file, "gemma/mmproj.gguf");
    EXPECT_EQ(infantry->files().size(), 2U);
    EXPECT_EQ(catalog.find_model("commander")->weights.url, "https://example.invalid/qwen.gguf");

    const Profile* big = catalog.find_profile("48gb");
    ASSERT_NE(big, nullptr);
    ASSERT_EQ(big->engines.size(), 3U);
    EXPECT_EQ(big->engines[0].extra_args, (std::vector<std::string>{"--flash-attn", "on"}));
    EXPECT_EQ(big->engines[2].slots, 1) << "defaults apply";
    EXPECT_EQ(big->engines[2].context, 8192);
}

TEST(Catalog, ChoosesTheMostDemandingProfileThatFits) {
    const Catalog catalog = unwrap(parse_catalog(catalog_text()));
    EXPECT_EQ(unwrap(choose_profile(catalog, 24564))->name, "24gb");  // RTX 4090
    EXPECT_EQ(unwrap(choose_profile(catalog, 46068))->name, "48gb");  // L40S
    EXPECT_EQ(unwrap(choose_profile(catalog, 81920))->name, "48gb");  // bigger still fits the biggest
    EXPECT_ERRC(choose_profile(catalog, 16000), Errc::unsupported);
}

TEST(Catalog, RefusesInvalidDefinitions) {
    const auto fails_with = [](const std::string& text, std::string_view message) {
        const auto result = parse_catalog(text, "test.toml");
        ASSERT_FALSE(result.has_value()) << "accepted: " << text;
        EXPECT_EQ(result.error().code(), Errc::invalid_argument);
        EXPECT_THAT(result.error().message(), HasSubstr(message));
    };
    const std::string model = std::format(R"([[model]]
id = "m"
role = "commander"
file = "m.gguf"
sha256 = "{}"
)",
                                          kSha);
    const std::string profile = "[[profile]]\nname = \"p\"\n  [[profile.engine]]\n  model = \"m\"\n";

    fails_with("[[model]\n", "test.toml: line 1");
    fails_with(model, "defines no [[profile]]");
    fails_with(model + model + profile, "model id 'm' appears twice");
    fails_with(
        std::format("[[model]]\nid = \"m\"\nrole = \"chef\"\nfile = \"m.gguf\"\nsha256 = \"{}\"\n", kSha) +
            profile,
        "unknown role 'chef'");
    fails_with(
        std::format("[[model]]\nid = \"m\"\nrole = \"infantry\"\nfile = \"../etc/passwd\"\nsha256 = \"{}\"\n",
                    kSha) +
            profile,
        "relative path without '..'");
    fails_with(
        std::format("[[model]]\nid = \"m\"\nrole = \"infantry\"\nfile = \"/abs.gguf\"\nsha256 = \"{}\"\n",
                    kSha) +
            profile,
        "relative path without '..'");
    fails_with("[[model]]\nid = \"m\"\nrole = \"infantry\"\nfile = \"m.gguf\"\nsha256 = \"ABC\"\n" + profile,
               "64 lowercase hex");
    fails_with(model + "[[profile]]\nname = \"p\"\n", "needs at least one [[profile.engine]]");
    fails_with(model + "[[profile]]\nname = \"p\"\n  [[profile.engine]]\n  model = \"ghost\"\n",
               "unknown model 'ghost'");
    fails_with(model +
                   "[[profile]]\nname = \"p\"\n  [[profile.engine]]\n  model = \"m\"\n  [[profile.engine]]\n "
                   " model = \"m\"\n",
               "already served");
    fails_with(model + "[[profile]]\nname = \"p\"\n  [[profile.engine]]\n  model = \"m\"\n  slots = 0\n",
               "slots must be");
    fails_with(model +
                   "[[profile]]\nname = \"p\"\n  [[profile.engine]]\n  model = \"m\"\n  slots = 64\n  "
                   "context = 1024\n",
               "under 256 tokens per slot");
    fails_with(model + profile + profile, "profile name 'p' appears twice");
}

TEST(Catalog, LoadsFromAFile) {
    const testkit::TempDir dir{"rr_catalog_test"};
    EXPECT_EQ(unwrap(load_catalog(dir.write_file("catalog.toml", catalog_text()))).profiles.size(), 2U);
    EXPECT_ERRC(load_catalog(dir / "missing.toml"), Errc::not_found);
}

TEST(LlamaServerCommand, ListensOnLocalhostAndCarriesTheProfile) {
    const Catalog catalog = unwrap(parse_catalog(catalog_text()));
    const Profile& big = *catalog.find_profile("48gb");
    const auto commander = llama_server_command("llama-server", "/workspace/models",
                                                *catalog.find_model("commander"), big.engines[0], 18080);
    EXPECT_THAT(commander,
                ElementsAre("llama-server", "--model", "/workspace/models/qwen/Qwen3.8-27B-Q4_K_M.gguf",
                            "--alias", "commander", "--host", "127.0.0.1", "--port", "18080", "--parallel",
                            "4", "--ctx-size", "32768", "--flash-attn", "on"));

    const auto infantry =
        llama_server_command("llama-server", "/m", *catalog.find_model("infantry"), big.engines[1], 1);
    EXPECT_THAT(infantry, ::testing::Contains("--mmproj"));
    EXPECT_THAT(infantry, ::testing::Contains("/m/gemma/mmproj.gguf"));
    const auto embedder =
        llama_server_command("llama-server", "/m", *catalog.find_model("embedder"), big.engines[2], 1);
    EXPECT_THAT(embedder, ::testing::Contains("--embedding"));
}

}  // namespace
}  // namespace rr::supervisor
