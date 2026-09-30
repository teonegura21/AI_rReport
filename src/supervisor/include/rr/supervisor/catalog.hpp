// rr/supervisor/catalog.hpp — the models a pod may run, and how, per GPU size.
//
// Why this exists
//   Which models run, with how many parallel slots and how much context, is
//   configuration (plan §3.10, §4.5), decided per GPU size by the M0
//   benchmark. One TOML file holds it all:
//
//     [[model]]
//     id = "qwen3.8-27b-q4km"                 # the name clients send as "model"
//     role = "commander"                      # commander | infantry | embedder | reranker
//     file = "qwen3.8/Qwen3.8-27B-Q4_K_M.gguf"  # relative to the models directory
//     sha256 = "…64 hex…"                     # verified before the model is used
//     url = "https://huggingface.co/…"        # where `models sync` downloads it
//     projector = { file = "…mmproj.gguf", sha256 = "…", url = "…" }  # vision models
//
//     [[profile]]
//     name = "48gb"
//     min_vram_mib = 44000                    # chosen when the GPU has at least this
//       [[profile.engine]]
//       model = "qwen3.8-27b-q4km"
//       slots = 4                             # parallel sequences (llama-server -np)
//       context = 32768                       # total context, shared by the slots
//       extra_args = ["--flash-attn", "on"]
//
//   The file is validated as a whole when loaded: unknown roles, duplicate
//   IDs, profiles naming unknown models, malformed checksums and paths that
//   could leave the models directory are all refused up front, not
//   discovered when an engine fails to start.
//
// Thread-safety: an immutable value once loaded.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>

namespace rr::supervisor {

struct ModelFile {
    std::filesystem::path file{};  // relative to the models directory
    std::string sha256{};          // 64 lowercase hex characters
    std::optional<std::string> url{};
};

struct Model {
    std::string id{};
    std::string role{};
    ModelFile weights{};
    std::optional<ModelFile> projector{};  // multimodal projector (llama-server --mmproj)

    // Every file the model needs.
    [[nodiscard]] std::vector<const ModelFile*> files() const;
};

struct EngineSpec {
    std::string model{};
    int slots = 1;
    int context = 8192;
    std::vector<std::string> extra_args{};
};

struct Profile {
    std::string name{};
    std::int64_t min_vram_mib = 0;
    std::vector<EngineSpec> engines{};
};

struct Catalog {
    std::vector<Model> models{};
    std::vector<Profile> profiles{};

    [[nodiscard]] const Model* find_model(std::string_view id) const noexcept;
    [[nodiscard]] const Profile* find_profile(std::string_view name) const noexcept;
};

inline constexpr std::array<std::string_view, 4> kRoles{"commander", "infantry", "embedder", "reranker"};

// Parses and validates a catalog. `origin` names the source in errors.
[[nodiscard]] Result<Catalog> parse_catalog(std::string_view toml_text, std::string_view origin = "catalog");
[[nodiscard]] Result<Catalog> load_catalog(const std::filesystem::path& path);

// The most demanding profile that fits `vram_mib` (largest min_vram_mib not
// above it). Errc::unsupported when none fits.
[[nodiscard]] Result<const Profile*> choose_profile(const Catalog& catalog, std::int64_t vram_mib);

// The llama-server command line serving `spec` on 127.0.0.1:`port`.
// Engines only ever listen on localhost; the supervisor is the one door in.
[[nodiscard]] std::vector<std::string> llama_server_command(const std::string& executable,
                                                            const std::filesystem::path& models_dir,
                                                            const Model& model, const EngineSpec& spec,
                                                            int port);

}  // namespace rr::supervisor
