#include <rr/supervisor/catalog.hpp>

#include <cstdint>
#include <filesystem>
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
    // Each case below changes one thing in a valid catalog of these parts.
    const auto model_entry = [](std::string_view role, std::string_view file, std::string_view sha) {
        return std::format("[[model]]\nid = \"m\"\nrole = \"{}\"\nfile = \"{}\"\nsha256 = \"{}\"\n", role,
                           file, sha);
    };
    const auto engine = [](std::string_view model_id, std::string_view settings = "") {
        return std::format("  [[profile.engine]]\n  model = \"{}\"\n{}", model_id, settings);
    };
    const std::string model = model_entry("commander", "m.gguf", kSha);
    const std::string profile_head = "[[profile]]\nname = \"p\"\n";
    const std::string profile = profile_head + engine("m");

    fails_with("[[model]\n", "test.toml: line 1");
    fails_with(model, "defines no [[profile]]");
    fails_with(model + model + profile, "model id 'm' appears twice");
    fails_with(model_entry("chef", "m.gguf", kSha) + profile, "unknown role 'chef'");
    fails_with(model_entry("infantry", "../etc/passwd", kSha) + profile, "relative path without '..'");
    fails_with(model_entry("infantry", "/abs.gguf", kSha) + profile, "relative path without '..'");
    fails_with(model_entry("infantry", "m.gguf", "ABC") + profile, "64 lowercase hex");
    fails_with(model + profile_head, "needs at least one [[profile.engine]]");
    fails_with(model + profile_head + engine("ghost"), "unknown model 'ghost'");
    fails_with(model + profile_head + engine("m") + engine("m"), "already served");
    fails_with(model + profile_head + engine("m", "  slots = 0\n"), "slots must be");
    fails_with(model + profile_head + engine("m", "  slots = 64\n  context = 1024\n"),
               "under 256 tokens per slot");
    fails_with(model + profile + profile, "profile name 'p' appears twice");
}

TEST(Catalog, LoadsFromAFile) {
    const testkit::TempDir dir{"rr_catalog_test"};
    EXPECT_EQ(unwrap(load_catalog(dir.write_file("catalog.toml", catalog_text()))).profiles.size(), 2U);
    EXPECT_ERRC(load_catalog(dir / "missing.toml"), Errc::not_found);
}

// The catalog the image ships (docker/runtime/catalog.toml) loads, and
// "auto" picks what its comments promise, for the memory real GPUs report.
TEST(Catalog, TheImagesCatalogChoosesAsDocumented) {
    const Catalog catalog =
        unwrap(load_catalog(std::filesystem::path{RR_SOURCE_DIR} / "docker/runtime/catalog.toml"));
    const auto chosen = [&catalog](std::int64_t vram_mib) {
        return unwrap(choose_profile(catalog, vram_mib))->name;
    };
    EXPECT_EQ(chosen(16'376), "16gb") << "RTX A4000";
    EXPECT_EQ(chosen(23'034), "16gb") << "L4: no commander fits";
    EXPECT_EQ(chosen(24'564), "24gb") << "RTX 4090: the commander, the first of two 24 GB profiles";
    EXPECT_EQ(chosen(32'607), "32gb") << "RTX 5090";
    EXPECT_EQ(chosen(46'068), "48gb") << "L40S";
    EXPECT_EQ(chosen(49'140), "48gb") << "RTX A6000";
    EXPECT_EQ(chosen(81'559), "80gb") << "H100";
    EXPECT_ERRC(choose_profile(catalog, 12'288), Errc::unsupported);
    EXPECT_NE(catalog.find_profile("24gb-infantry"), nullptr) << "chosen by name";

    for (const Model& model : catalog.models) {
        for (const ModelFile* file : model.files()) {
            ASSERT_TRUE(file->url.has_value()) << model.id << ": models sync must be able to fetch it";
            EXPECT_TRUE(file->url->starts_with("https://huggingface.co/")) << *file->url;
        }
    }
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
