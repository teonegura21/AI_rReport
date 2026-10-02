#include <rr/supervisor/catalog.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

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
  kv_unified = true
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
    EXPECT_TRUE(big->engines[0].kv_unified);
    EXPECT_FALSE(big->engines[1].kv_unified) << "an equal share per slot unless the catalog says otherwise";
}

TEST(Catalog, ContextPerSlotIsAShareOrTheWholePool) {
    const Catalog catalog = unwrap(parse_catalog(catalog_text()));
    const Profile& big = *catalog.find_profile("48gb");
    EXPECT_EQ(planned_context_per_slot(big.engines[0]), 32768) << "4 slots sharing one pool of 32768";
    EXPECT_EQ(planned_context_per_slot(big.engines[1]), 4096) << "16 slots, 65536 split evenly";
    EXPECT_EQ(planned_context_per_slot(big.engines[2]), 8192) << "one slot has it all";
    EXPECT_EQ(planned_context_per_slot(EngineSpec{.slots = 3, .context = 1000}), 333)
        << "llama-server's integer division";
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

    // Unknown keys and wrong types are refused, never ignored for a default.
    fails_with(model + profile_head + engine("m", "  kv_unifed = true\n"),
               "unknown key 'kv_unifed' (known: model, slots, context, kv_unified, extra_args)");
    fails_with(model + "quant = \"q4\"\n" + profile, "model #1: unknown key 'quant'");
    fails_with(model + profile_head + "vram = 1\n" + engine("m"), "profile #1: unknown key 'vram'");
    fails_with("[[models]]\nid = \"m\"\n" + profile, "test.toml: unknown key 'models'");
    fails_with("[model]\nid = \"m\"\n" + profile, "'model' must be [[model]] tables");
    fails_with("model = [1, 2]\n" + profile, "'model' must be [[model]] tables");
    fails_with(model + profile_head + engine("m", "  slots = \"4\"\n"), "'slots' must be an integer");
    fails_with(model + profile_head + engine("m", "  context = 8192.0\n"), "'context' must be an integer");
    fails_with(model + profile_head + engine("m", "  kv_unified = \"yes\"\n"),
               "'kv_unified' must be true or false");
    fails_with(model + "[[profile]]\nname = \"p\"\nmin_vram_mib = \"44000\"\n" + engine("m"),
               "'min_vram_mib' must be an integer");
    fails_with(model + "[[profile]]\nname = \"p\"\nmin_vram_mib = -1\n" + engine("m"),
               "min_vram_mib must not be negative");
    fails_with(model + "url = 3\n" + profile, "'url' must be a string");
    fails_with(model + "projector = \"mmproj.gguf\"\n" + profile, "projector: must be a table");
    fails_with(model + std::format(R"(projector = {{ file = "p.gguf", sha256 = "{}", size = 1 }})", kSha) +
                   "\n" + profile,
               "projector: unknown key 'size'");
    fails_with(model + profile_head + engine("m", "  extra_args = \"--jinja\"\n"),
               "extra_args must be an array of strings");
    fails_with(model + profile_head + engine("m", "  context = 4294967296\n"),
               "context 4294967296 is too large");

    // extra_args may not overrule the catalog, nor load what it has not verified.
    for (const std::string_view flag : {"--parallel",
                                        "-np",
                                        "--ctx-size",
                                        "--ctx_size",
                                        "-c",
                                        "--kv-unified",
                                        "--no-kv-unified",
                                        "--kv-unified-per-slot",
                                        "--model",
                                        "--alias",
                                        "--port",
                                        "--host",
                                        "--mmproj",
                                        "--embedding",
                                        "-hf",
                                        "--hf-repo",
                                        "--model-url",
                                        "--lora",
                                        "--model-draft",
                                        "--api-key"}) {
        fails_with(model + profile_head + engine("m", std::format("  extra_args = [\"{}\", \"8\"]\n", flag)),
                   std::format("extra_args may not set '{}'", flag));
    }
}

TEST(Catalog, ExtraArgsMayTuneTheEngine) {
    const std::string text = std::format(R"toml(
[[model]]
id = "m"
role = "infantry"
file = "m.gguf"
sha256 = "{}"
[[profile]]
name = "p"
  [[profile.engine]]
  model = "m"
  extra_args = ["--n-gpu-layers", "999", "--flash-attn", "on", "--cache-type-k", "q8_0", "--jinja",
                "--ctx-checkpoints", "8", "--cache-ram", "8192", "-np-not-a-flag"]
)toml",
                                         kSha);
    EXPECT_EQ(unwrap(parse_catalog(text)).profiles[0].engines[0].extra_args.size(), 12U)
        << "only the reserved flags are refused";
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

    // Where the pipeline runs (everything resident), the commander takes
    // several requests at once and its slots share one pool: k drafts read
    // their prompt once, and each may use the whole context.
    for (const std::string_view name : {"48gb", "80gb"}) {
        const Profile& profile = *catalog.find_profile(name);
        const auto commander = std::ranges::find_if(profile.engines, [&catalog](const EngineSpec& engine) {
            return catalog.find_model(engine.model)->role == "commander";
        });
        ASSERT_NE(commander, profile.engines.end()) << name;
        EXPECT_EQ(commander->slots, 4) << name;
        EXPECT_TRUE(commander->kv_unified) << name;
        EXPECT_EQ(planned_context_per_slot(*commander), commander->context) << name;
    }
}

// The MTP twins (P29): each `-mtp` profile is its plain twin's, at the same
// size and listed after it, so that "auto" never picks it, and the commander
// alone drafts with its own multi-token-prediction head. What the runbook's
// benchmark compares between the two is then the head, and nothing else.
TEST(Catalog, EachMtpTwinIsItsProfileWithTheCommanderSpeculating) {
    const Catalog catalog =
        unwrap(load_catalog(std::filesystem::path{RR_SOURCE_DIR} / "docker/runtime/catalog.toml"));
    const std::vector<std::string> speculating{"--spec-type", "draft-mtp", "--spec-draft-n-max", "3"};
    std::vector<std::string> twins;
    for (std::size_t index = 0; index < catalog.profiles.size(); ++index) {
        const Profile& twin = catalog.profiles[index];
        if (!twin.name.ends_with("-mtp")) {
            continue;
        }
        twins.push_back(twin.name);
        const std::string plain = twin.name.substr(0, twin.name.size() - std::string_view{"-mtp"}.size());
        const auto found = std::ranges::find(catalog.profiles, plain, &Profile::name);
        ASSERT_NE(found, catalog.profiles.end()) << twin.name << " has a profile to be the twin of";
        EXPECT_LT(static_cast<std::size_t>(found - catalog.profiles.begin()), index)
            << twin.name << ": listed after " << plain << ", which wins the tie";
        EXPECT_EQ(twin.min_vram_mib, found->min_vram_mib) << twin.name;
        ASSERT_EQ(twin.engines.size(), found->engines.size()) << twin.name;
        for (std::size_t engine = 0; engine < twin.engines.size(); ++engine) {
            EngineSpec expected = found->engines[engine];
            if (catalog.find_model(expected.model)->role == "commander") {
                expected.extra_args.insert(expected.extra_args.end(), speculating.begin(), speculating.end());
            }
            EXPECT_EQ(twin.engines[engine], expected) << twin.name << ", engine " << engine + 1;
        }
    }
    EXPECT_THAT(twins, ElementsAre("48gb-mtp", "80gb-mtp"));
}

TEST(LlamaServerCommand, ListensOnLocalhostAndCarriesTheProfile) {
    const Catalog catalog = unwrap(parse_catalog(catalog_text()));
    const Profile& big = *catalog.find_profile("48gb");
    const auto commander = llama_server_command("llama-server", "/workspace/models",
                                                *catalog.find_model("commander"), big.engines[0], 18080);
    EXPECT_THAT(commander,
                ElementsAre("llama-server", "--model", "/workspace/models/qwen/Qwen3.8-27B-Q4_K_M.gguf",
                            "--alias", "commander", "--host", "127.0.0.1", "--port", "18080", "--parallel",
                            "4", "--ctx-size", "32768", "--kv-unified", "--flash-attn", "on"));

    const auto infantry =
        llama_server_command("llama-server", "/m", *catalog.find_model("infantry"), big.engines[1], 1);
    EXPECT_THAT(infantry, ::testing::Contains("--no-kv-unified"))
        << "stated, so LLAMA_ARG_KV_UNIFIED in the environment cannot turn it on";
    EXPECT_THAT(infantry, ::testing::Contains("--mmproj"));
    EXPECT_THAT(infantry, ::testing::Contains("/m/gemma/mmproj.gguf"));
    const auto embedder =
        llama_server_command("llama-server", "/m", *catalog.find_model("embedder"), big.engines[2], 1);
    EXPECT_THAT(embedder, ::testing::Contains("--embedding"));
}

}  // namespace
}  // namespace rr::supervisor
