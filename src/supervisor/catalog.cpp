#include <rr/supervisor/catalog.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>
#include <utility>

#include <toml++/toml.hpp>

namespace rr::supervisor {

namespace {

// Flags extra_args may not carry, as llama.cpp 7fe450e spells them:
//   * the supervisor's own: it sets them from the catalog's fields, and
//     llama-server keeps the last of two values, so a second one would
//     silently overrule the catalog (and the health report would be wrong);
//   * flags that make the engine load or fetch a file the catalog has not
//     verified;
//   * an API key, which would make the engine refuse the supervisor's relay
//     (it sends none: engines listen on localhost only, behind the door).
constexpr auto kReservedFlags = std::to_array<std::string_view>(
    {// the supervisor's own
     "-m", "--model", "-a", "--alias", "--host", "--port", "-np", "--parallel", "-c", "--ctx-size", "-kvu",
     "--kv-unified", "-no-kvu", "--no-kv-unified", "--kv-unified-per-slot", "-mm", "--mmproj", "--embedding",
     "--embeddings", "--rerank", "--reranking",
     // files the catalog has not verified
     "-hf", "-hfr", "--hf-repo", "-hff", "--hf-file", "-mu", "--model-url", "-dr", "--docker-repo", "-mmu",
     "--mmproj-url", "--lora", "--lora-scaled", "-md", "--model-draft", "--spec-draft-model",
     // the engine's own door
     "--api-key", "--api-key-file"});

std::unexpected<Error> invalid(std::string_view origin, std::string message) {
    return fail(Errc::invalid_argument, std::format("{}: {}", origin, message));
}

// llama.cpp's reading of a flag: in a "--" flag, '_' and '-' are the same.
std::string normalized_flag(std::string_view arg) {
    std::string flag{arg};
    if (flag.starts_with("--")) {
        std::ranges::replace(flag, '_', '-');
    }
    return flag;
}

// Refuses keys a table of this kind does not have: a misspelt optional key
// would otherwise be ignored, and its default silently used.
Result<void> known_keys_only(const toml::table& table, std::initializer_list<std::string_view> known,
                             std::string_view where) {
    for (const auto& entry : table) {
        const std::string_view key = entry.first.str();
        if (std::ranges::find(known, key) == known.end()) {
            std::string names;
            for (const std::string_view name : known) {
                names += names.empty() ? std::string{name} : std::format(", {}", name);
            }
            return fail(Errc::invalid_argument,
                        std::format("{}: unknown key '{}' (known: {})", where, key, names));
        }
    }
    return ok();
}

// Optional values: `fallback` (or nothing) when absent, an error when present
// with another type: a quoted number must not become the default.
Result<std::int64_t> integer_or(const toml::table& table, std::string_view key, std::int64_t fallback,
                                std::string_view where) {
    const toml::node* node = table.get(key);
    if (node == nullptr) {
        return fallback;
    }
    const auto* integer = node->as_integer();
    if (integer == nullptr) {
        return fail(Errc::invalid_argument, std::format("{}: '{}' must be an integer", where, key));
    }
    return integer->get();
}

Result<bool> boolean_or(const toml::table& table, std::string_view key, bool fallback,
                        std::string_view where) {
    const toml::node* node = table.get(key);
    if (node == nullptr) {
        return fallback;
    }
    const auto* boolean = node->as_boolean();
    if (boolean == nullptr) {
        return fail(Errc::invalid_argument, std::format("{}: '{}' must be true or false", where, key));
    }
    return boolean->get();
}

Result<std::optional<std::string>> optional_string(const toml::table& table, std::string_view key,
                                                   std::string_view where) {
    const toml::node* node = table.get(key);
    if (node == nullptr) {
        return std::optional<std::string>{};
    }
    const auto* text = node->as_string();
    if (text == nullptr) {
        return fail(Errc::invalid_argument, std::format("{}: '{}' must be a string", where, key));
    }
    return std::optional<std::string>{text->get()};
}

// The tables of an array of tables, written `syntax` ("[[model]]"): none
// when the key is absent.
Result<std::vector<const toml::table*>> tables_at(const toml::table& table, std::string_view key,
                                                  std::string_view syntax, std::string_view where) {
    std::vector<const toml::table*> tables;
    const toml::node* node = table.get(key);
    if (node == nullptr) {
        return tables;
    }
    const toml::array* array = node->as_array();
    if (array == nullptr || !array->is_array_of_tables()) {
        return fail(Errc::invalid_argument, std::format("{}: '{}' must be {} tables", where, key, syntax));
    }
    for (const toml::node& element : *array) {
        tables.push_back(element.as_table());
    }
    return tables;
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
    RR_TRY_ASSIGN(file.url, optional_string(table, "url", where));
    return file;
}

Result<Model> parse_model(const toml::table& table, std::string_view origin, std::size_t index) {
    const std::string where = std::format("{}: model #{}", origin, index + 1);
    RR_TRY(known_keys_only(table, {"id", "role", "file", "sha256", "url", "projector"}, where));
    Model model;
    RR_TRY_ASSIGN(model.id, required_string(table, "id", where));
    RR_TRY_ASSIGN(model.role, required_string(table, "role", where));
    if (std::ranges::find(kRoles, model.role) == kRoles.end()) {
        return fail(Errc::invalid_argument,
                    std::format("{} ('{}'): unknown role '{}' (commander, infantry, embedder or reranker)",
                                where, model.id, model.role));
    }
    RR_TRY_ASSIGN(model.weights, parse_model_file(table, std::format("{} ('{}')", where, model.id)));
    if (const toml::node* node = table.get("projector"); node != nullptr) {
        const std::string projector_where = std::format("{} ('{}') projector", where, model.id);
        const toml::table* projector = node->as_table();
        if (projector == nullptr) {
            return fail(
                Errc::invalid_argument,
                std::format("{}: must be a table: {{ file = …, sha256 = …, url = … }}", projector_where));
        }
        RR_TRY(known_keys_only(*projector, {"file", "sha256", "url"}, projector_where));
        RR_TRY_ASSIGN(model.projector, parse_model_file(*projector, projector_where));
    }
    return model;
}

Result<std::vector<std::string>> parse_extra_args(const toml::table& table, std::string_view where) {
    std::vector<std::string> args;
    const toml::node* node = table.get("extra_args");
    if (node == nullptr) {
        return args;
    }
    const toml::array* array = node->as_array();
    if (array == nullptr) {
        return fail(Errc::invalid_argument, std::format("{}: extra_args must be an array of strings", where));
    }
    for (const toml::node& arg : *array) {
        const auto* text = arg.as_string();
        if (text == nullptr) {
            return fail(Errc::invalid_argument, std::format("{}: extra_args must be strings", where));
        }
        if (std::ranges::find(kReservedFlags, normalized_flag(text->get())) != kReservedFlags.end()) {
            return fail(Errc::invalid_argument,
                        std::format("{}: extra_args may not set '{}': the supervisor sets the model, "
                                    "address, slots, context and pool from the catalog's own fields, and "
                                    "engines load only the files the catalog verifies",
                                    where, text->get()));
        }
        args.push_back(text->get());
    }
    return args;
}

Result<EngineSpec> parse_engine(const toml::table& table, std::string_view where) {
    RR_TRY(known_keys_only(table, {"model", "slots", "context", "kv_unified", "extra_args"}, where));
    EngineSpec engine;
    RR_TRY_ASSIGN(engine.model, required_string(table, "model", where));
    RR_TRY_ASSIGN(const std::int64_t slots, integer_or(table, "slots", engine.slots, where));
    if (slots < 1 || slots > 256) {
        return fail(Errc::invalid_argument, std::format("{}: slots must be between 1 and 256", where));
    }
    engine.slots = static_cast<int>(slots);
    RR_TRY_ASSIGN(const std::int64_t context, integer_or(table, "context", engine.context, where));
    // Every slot needs room for at least a modest prompt, even when all are
    // at work in a shared pool.
    if (context < std::int64_t{256} * engine.slots) {
        return fail(Errc::invalid_argument,
                    std::format("{}: context {} leaves under 256 tokens per slot ({} slots)", where, context,
                                engine.slots));
    }
    if (!std::in_range<int>(context)) {
        return fail(Errc::invalid_argument, std::format("{}: context {} is too large", where, context));
    }
    engine.context = static_cast<int>(context);
    RR_TRY_ASSIGN(engine.kv_unified, boolean_or(table, "kv_unified", engine.kv_unified, where));
    RR_TRY_ASSIGN(engine.extra_args, parse_extra_args(table, where));
    return engine;
}

Result<Profile> parse_profile(const toml::table& table, const Catalog& catalog, std::string_view origin,
                              std::size_t index) {
    const std::string where = std::format("{}: profile #{}", origin, index + 1);
    RR_TRY(known_keys_only(table, {"name", "min_vram_mib", "engine"}, where));
    Profile profile;
    RR_TRY_ASSIGN(profile.name, required_string(table, "name", where));
    const std::string named = std::format("{} ('{}')", where, profile.name);
    RR_TRY_ASSIGN(profile.min_vram_mib, integer_or(table, "min_vram_mib", 0, named));
    if (profile.min_vram_mib < 0) {
        return fail(Errc::invalid_argument, std::format("{}: min_vram_mib must not be negative", named));
    }
    RR_TRY_ASSIGN(const std::vector<const toml::table*> engines,
                  tables_at(table, "engine", "[[profile.engine]]", named));
    if (engines.empty()) {
        return fail(Errc::invalid_argument, std::format("{}: needs at least one [[profile.engine]]", named));
    }
    std::set<std::string> served;
    for (std::size_t engine_index = 0; engine_index < engines.size(); ++engine_index) {
        const std::string engine_where = std::format("{} engine #{}", named, engine_index + 1);
        RR_TRY_ASSIGN(EngineSpec engine, parse_engine(*engines[engine_index], engine_where));
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

int planned_context_per_slot(const EngineSpec& spec) noexcept {
    return spec.kv_unified ? spec.context : spec.context / std::max(spec.slots, 1);
}

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

    RR_TRY(known_keys_only(document, {"model", "profile"}, origin));
    Catalog catalog;
    RR_TRY_ASSIGN(const std::vector<const toml::table*> models,
                  tables_at(document, "model", "[[model]]", origin));
    for (std::size_t index = 0; index < models.size(); ++index) {
        RR_TRY_ASSIGN(Model model, parse_model(*models[index], origin, index));
        if (catalog.find_model(model.id) != nullptr) {
            return invalid(origin, std::format("model id '{}' appears twice", model.id));
        }
        catalog.models.push_back(std::move(model));
    }
    RR_TRY_ASSIGN(const std::vector<const toml::table*> profiles,
                  tables_at(document, "profile", "[[profile]]", origin));
    for (std::size_t index = 0; index < profiles.size(); ++index) {
        RR_TRY_ASSIGN(Profile profile, parse_profile(*profiles[index], catalog, origin, index));
        if (catalog.find_profile(profile.name) != nullptr) {
            return invalid(origin, std::format("profile name '{}' appears twice", profile.name));
        }
        catalog.profiles.push_back(std::move(profile));
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
                                     std::to_string(spec.context),
                                     spec.kv_unified ? "--kv-unified" : "--no-kv-unified"};
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
