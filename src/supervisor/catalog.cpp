#include <rr/supervisor/catalog.hpp>

#include <algorithm>
#include <format>
#include <fstream>
#include <set>
#include <sstream>

#include <toml++/toml.hpp>

namespace rr::supervisor {

namespace {

std::unexpected<Error> invalid(std::string_view origin, std::string message) {
    return fail(Errc::invalid_argument, std::format("{}: {}", origin, message));
}

bool is_sha256_hex(std::string_view text) {
    return text.size() == 64 &&
           std::ranges::all_of(text, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// A path that stays inside the models directory: relative, and without '..'.
bool is_contained_path(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) {
        return false;
    }
    return std::ranges::none_of(path, [](const std::filesystem::path& part) { return part == ".."; });
}

Result<std::string> required_string(const toml::table& table, std::string_view key, std::string_view where) {
    const auto value = table[key].value<std::string>();
    if (!value.has_value() || value->empty()) {
        return fail(Errc::invalid_argument, std::format("{}: '{}' must be a non-empty string", where, key));
    }
    return *value;
}

Result<ModelFile> parse_model_file(const toml::table& table, std::string_view where) {
    ModelFile file;
    RR_TRY_ASSIGN(const std::string path, required_string(table, "file", where));
    file.file = path;
    if (!is_contained_path(file.file)) {
        return fail(Errc::invalid_argument,
                    std::format("{}: file '{}' must be a relative path without '..'", where, path));
    }
    RR_TRY_ASSIGN(file.sha256, required_string(table, "sha256", where));
    if (!is_sha256_hex(file.sha256)) {
        return fail(Errc::invalid_argument,
                    std::format("{}: sha256 must be 64 lowercase hex characters", where));
    }
    file.url = table["url"].value<std::string>();
    return file;
}

Result<Model> parse_model(const toml::table& table, std::string_view origin, std::size_t index) {
    const std::string where = std::format("{}: model #{}", origin, index + 1);
    Model model;
    RR_TRY_ASSIGN(model.id, required_string(table, "id", where));
    RR_TRY_ASSIGN(model.role, required_string(table, "role", where));
    if (std::ranges::find(kRoles, model.role) == kRoles.end()) {
        return fail(Errc::invalid_argument,
                    std::format("{} ('{}'): unknown role '{}' (commander, infantry, embedder or reranker)",
                                where, model.id, model.role));
    }
    RR_TRY_ASSIGN(model.weights, parse_model_file(table, std::format("{} ('{}')", where, model.id)));
    if (const toml::table* projector = table["projector"].as_table(); projector != nullptr) {
        RR_TRY_ASSIGN(model.projector,
                      parse_model_file(*projector, std::format("{} ('{}') projector", where, model.id)));
    }
    return model;
}

Result<EngineSpec> parse_engine(const toml::table& table, std::string_view where) {
    EngineSpec engine;
    RR_TRY_ASSIGN(engine.model, required_string(table, "model", where));
    engine.slots = table["slots"].value<int>().value_or(1);
    engine.context = table["context"].value<int>().value_or(8192);
    if (engine.slots < 1 || engine.slots > 256) {
        return fail(Errc::invalid_argument, std::format("{}: slots must be between 1 and 256", where));
    }
    // Every slot needs room for at least a modest prompt.
    if (engine.context < 256 * engine.slots) {
        return fail(Errc::invalid_argument,
                    std::format("{}: context {} leaves under 256 tokens per slot ({} slots)", where,
                                engine.context, engine.slots));
    }
    if (const toml::array* args = table["extra_args"].as_array(); args != nullptr) {
        for (const toml::node& arg : *args) {
            const auto text = arg.value<std::string>();
            if (!text.has_value()) {
                return fail(Errc::invalid_argument, std::format("{}: extra_args must be strings", where));
            }
            engine.extra_args.push_back(*text);
        }
    }
    return engine;
}

Result<Profile> parse_profile(const toml::table& table, const Catalog& catalog, std::string_view origin,
                              std::size_t index) {
    const std::string where = std::format("{}: profile #{}", origin, index + 1);
    Profile profile;
    RR_TRY_ASSIGN(profile.name, required_string(table, "name", where));
    profile.min_vram_mib = table["min_vram_mib"].value<std::int64_t>().value_or(0);
    const toml::array* engines = table["engine"].as_array();
    if (engines == nullptr || engines->empty()) {
        return fail(Errc::invalid_argument,
                    std::format("{} ('{}'): needs at least one [[profile.engine]]", where, profile.name));
    }
    std::set<std::string> served;
    for (std::size_t engine_index = 0; engine_index < engines->size(); ++engine_index) {
        const toml::table* engine_table = (*engines)[engine_index].as_table();
        const std::string engine_where =
            std::format("{} ('{}') engine #{}", where, profile.name, engine_index + 1);
        if (engine_table == nullptr) {
            return fail(Errc::invalid_argument, std::format("{}: must be a table", engine_where));
        }
        RR_TRY_ASSIGN(EngineSpec engine, parse_engine(*engine_table, engine_where));
        if (catalog.find_model(engine.model) == nullptr) {
            return fail(Errc::invalid_argument,
                        std::format("{}: unknown model '{}'", engine_where, engine.model));
        }
        if (!served.insert(engine.model).second) {
            return fail(
                Errc::invalid_argument,
                std::format("{}: model '{}' is already served by this profile", engine_where, engine.model));
        }
        profile.engines.push_back(std::move(engine));
    }
    return profile;
}

}  // namespace

std::vector<const ModelFile*> Model::files() const {
    std::vector<const ModelFile*> all{&weights};
    if (projector.has_value()) {
        all.push_back(&*projector);
    }
    return all;
}

const Model* Catalog::find_model(std::string_view id) const noexcept {
    const auto found = std::ranges::find(models, id, &Model::id);
    return found == models.end() ? nullptr : &*found;
}

const Profile* Catalog::find_profile(std::string_view name) const noexcept {
    const auto found = std::ranges::find(profiles, name, &Profile::name);
    return found == profiles.end() ? nullptr : &*found;
}

Result<Catalog> parse_catalog(std::string_view toml_text, std::string_view origin) {
    toml::table document;
    try {
        document = toml::parse(toml_text, origin);
    } catch (const toml::parse_error& error) {
        return invalid(origin, std::format("line {}: {}", error.source().begin.line, error.description()));
    }

    Catalog catalog;
    if (const toml::array* models = document["model"].as_array(); models != nullptr) {
        for (std::size_t index = 0; index < models->size(); ++index) {
            const toml::table* table = (*models)[index].as_table();
            if (table == nullptr) {
                return invalid(origin, std::format("model #{} must be a table", index + 1));
            }
            RR_TRY_ASSIGN(Model model, parse_model(*table, origin, index));
            if (catalog.find_model(model.id) != nullptr) {
                return invalid(origin, std::format("model id '{}' appears twice", model.id));
            }
            catalog.models.push_back(std::move(model));
        }
    }
    if (const toml::array* profiles = document["profile"].as_array(); profiles != nullptr) {
        for (std::size_t index = 0; index < profiles->size(); ++index) {
            const toml::table* table = (*profiles)[index].as_table();
            if (table == nullptr) {
                return invalid(origin, std::format("profile #{} must be a table", index + 1));
            }
            RR_TRY_ASSIGN(Profile profile, parse_profile(*table, catalog, origin, index));
            if (catalog.find_profile(profile.name) != nullptr) {
                return invalid(origin, std::format("profile name '{}' appears twice", profile.name));
            }
            catalog.profiles.push_back(std::move(profile));
        }
    }
    if (catalog.profiles.empty()) {
        return invalid(origin, "defines no [[profile]]");
    }
    return catalog;
}

Result<Catalog> load_catalog(const std::filesystem::path& path) {
    const std::ifstream file(path, std::ios::binary);
    if (!file) {
        return fail(Errc::not_found, std::format("cannot read the model catalog '{}'", path.string()));
    }
    std::ostringstream text;
    text << file.rdbuf();
    return parse_catalog(text.str(), path.string());
}

Result<const Profile*> choose_profile(const Catalog& catalog, std::int64_t vram_mib) {
    const Profile* best = nullptr;
    for (const Profile& profile : catalog.profiles) {
        if (profile.min_vram_mib <= vram_mib &&
            (best == nullptr || profile.min_vram_mib > best->min_vram_mib)) {
            best = &profile;
        }
    }
    if (best == nullptr) {
        return fail(Errc::unsupported, std::format("no GPU profile fits {} MiB of VRAM", vram_mib));
    }
    return best;
}

std::vector<std::string> llama_server_command(const std::string& executable,
                                              const std::filesystem::path& models_dir, const Model& model,
                                              const EngineSpec& spec, int port) {
    std::vector<std::string> command{executable,
                                     "--model",
                                     (models_dir / model.weights.file).string(),
                                     "--alias",
                                     model.id,
                                     "--host",
                                     "127.0.0.1",
                                     "--port",
                                     std::to_string(port),
                                     "--parallel",
                                     std::to_string(spec.slots),
                                     "--ctx-size",
                                     std::to_string(spec.context)};
    if (model.projector.has_value()) {
        command.insert(command.end(), {"--mmproj", (models_dir / model.projector->file).string()});
    }
    if (model.role == "embedder") {
        command.emplace_back("--embedding");
    } else if (model.role == "reranker") {
        command.emplace_back("--reranking");
    }
    command.insert(command.end(), spec.extra_args.begin(), spec.extra_args.end());
    return command;
}

}  // namespace rr::supervisor
