#include <rr/prompt/library.hpp>

#include <algorithm>
#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

#include <inja/inja.hpp>
#include <toml++/toml.hpp>

#include <rr/core/assert.hpp>
#include <rr/core/hash.hpp>
#include <rr/embedded/prompts.hpp>
#include <rr/json/schema.hpp>

namespace rr::prompt {

namespace {

constexpr std::string_view kManifest = "prompts/manifest.toml";

std::string system_path(std::string_view name) {
    return std::format("prompts/{}/system.md.inja", name);
}
std::string task_path(std::string_view name) {
    return std::format("prompts/{}/task.md.inja", name);
}
std::string example_path(std::string_view name) {
    return std::format("prompts/{}/example.json", name);
}

// The variables a template reads from its data: the first part of each name
// it uses ("brief" for {{ brief.speakers }}), less the ones it makes itself
// (a loop's variables, a {% set %}, inja's `loop`). Walks every branch.
class VariableCollector final : public inja::NodeVisitor {
public:
    [[nodiscard]] std::set<std::string> external() const {
        std::set<std::string> result;
        std::ranges::set_difference(used_, local_, std::inserter(result, result.end()));
        return result;
    }

private:
    void visit(const inja::BlockNode& node) override {
        for (const auto& child : node.nodes) {
            child->accept(*this);
        }
    }
    void visit(const inja::TextNode& /*node*/) override {}
    void visit(const inja::ExpressionNode& /*node*/) override {}
    void visit(const inja::LiteralNode& /*node*/) override {}
    void visit(const inja::DataNode& node) override {
        used_.insert(node.name.substr(0, node.name.find('.')));
    }
    void visit(const inja::FunctionNode& node) override {
        for (const auto& argument : node.arguments) {
            argument->accept(*this);
        }
    }
    void visit(const inja::ExpressionListNode& node) override {
        if (node.root) {
            node.root->accept(*this);
        }
    }
    void visit(const inja::StatementNode& /*node*/) override {}
    void visit(const inja::ForStatementNode& node) override {
        node.condition.accept(*this);
        node.body.accept(*this);
    }
    void visit(const inja::ForArrayStatementNode& node) override {
        local_.insert(node.value);
        local_.insert("loop");
        node.condition.accept(*this);
        node.body.accept(*this);
    }
    void visit(const inja::ForObjectStatementNode& node) override {
        local_.insert(node.key);
        local_.insert(node.value);
        local_.insert("loop");
        node.condition.accept(*this);
        node.body.accept(*this);
    }
    void visit(const inja::IfStatementNode& node) override {
        node.condition.accept(*this);
        node.true_statement.accept(*this);
        node.false_statement.accept(*this);
    }
    void visit(const inja::IncludeStatementNode& /*node*/) override {}
    void visit(const inja::ExtendsStatementNode& /*node*/) override {}
    void visit(const inja::BlockStatementNode& node) override { node.block.accept(*this); }
    void visit(const inja::SetStatementNode& node) override {
        local_.insert(node.key.substr(0, node.key.find('.')));
        node.expression.accept(*this);
    }

    std::set<std::string> used_;
    std::set<std::string> local_;
};

// An environment for prompts: whitespace as Markdown wants it, and nothing
// read from the filesystem behind the library's back.
inja::Environment environment() {
    inja::Environment env;
    env.set_trim_blocks(true);
    env.set_lstrip_blocks(true);
    env.set_search_included_templates_in_files(false);
    env.set_throw_at_missing_includes(true);
    return env;
}

Result<inja::Template> parse(inja::Environment& env, std::string_view text, std::string_view path) {
    try {
        return env.parse(text);
    } catch (const inja::InjaError& error) {
        return fail(Errc::invalid_argument,
                    std::format("{}: line {}, column {}: {}", path, error.location.line,
                                error.location.column, error.message));
    }
}

Result<std::string> render_template(inja::Environment& env, std::string_view text, const json::Json& data,
                                    std::string_view path) {
    RR_TRY_ASSIGN(const inja::Template parsed, parse(env, text, path));
    try {
        return env.render(parsed, data);
    } catch (const inja::InjaError& error) {
        return fail(Errc::invalid_argument,
                    std::format("{}: line {}, column {}: {}", path, error.location.line,
                                error.location.column, error.message));
    }
}

Result<std::vector<std::string>> strings(const toml::table& table, std::string_view key,
                                         std::string_view where) {
    std::vector<std::string> values;
    const toml::node* node = table.get(key);
    if (node == nullptr) {
        return values;
    }
    const toml::array* array = node->as_array();
    if (array == nullptr) {
        return fail(Errc::invalid_argument, std::format("{}: '{}' must be an array of strings", where, key));
    }
    for (const toml::node& element : *array) {
        const auto* text = element.as_string();
        if (text == nullptr) {
            return fail(Errc::invalid_argument,
                        std::format("{}: '{}' must be an array of strings", where, key));
        }
        values.push_back(text->get());
    }
    return values;
}

Result<std::string> string_at(const toml::table& table, std::string_view key, std::string_view where) {
    const toml::node* node = table.get(key);
    const auto* text = node == nullptr ? nullptr : node->as_string();
    if (text == nullptr || text->get().empty()) {
        return fail(Errc::invalid_argument, std::format("{}: '{}' must be a non-empty string", where, key));
    }
    return text->get();
}

Result<Version> parse_version(const toml::table& table, std::string_view where) {
    Version version;
    const toml::node* number = table.get("number");
    const auto* integer = number == nullptr ? nullptr : number->as_integer();
    if (integer == nullptr || !std::in_range<int>(integer->get())) {
        return fail(Errc::invalid_argument, std::format("{}: 'number' must be an integer", where));
    }
    version.number = static_cast<int>(integer->get());
    RR_TRY_ASSIGN(version.hash, string_at(table, "hash", where));
    if (const toml::node* note = table.get("note"); note != nullptr && note->as_string() != nullptr) {
        version.note = note->as_string()->get();
    }
    return version;
}

Result<PromptInfo> parse_prompt(const toml::table& table, std::size_t index) {
    const std::string where = std::format("{}: prompt #{}", kManifest, index + 1);
    PromptInfo info;
    RR_TRY_ASSIGN(info.name, string_at(table, "name", where));
    const std::string named = std::format("{} ('{}')", where, info.name);
    RR_TRY_ASSIGN(info.purpose, string_at(table, "purpose", named));
    if (table.get("schema") != nullptr) {
        RR_TRY_ASSIGN(info.schema, string_at(table, "schema", named));
    }
    RR_TRY_ASSIGN(info.shared, strings(table, "shared", named));
    RR_TRY_ASSIGN(info.failure_modes, strings(table, "failure_modes", named));
    const toml::array* versions = table["version"].as_array();
    if (versions == nullptr || versions->empty() || !versions->is_array_of_tables()) {
        return fail(Errc::invalid_argument, std::format("{}: needs at least one [[prompt.version]]", named));
    }
    for (std::size_t version = 0; version < versions->size(); ++version) {
        RR_TRY_ASSIGN(Version parsed, parse_version(*(*versions)[version].as_table(),
                                                    std::format("{} version #{}", named, version + 1)));
        info.versions.push_back(std::move(parsed));
    }
    return info;
}

Result<std::vector<PromptInfo>> parse_manifest(std::string_view text) {
    toml::table document;
    try {
        document = toml::parse(text, kManifest);
    } catch (const toml::parse_error& error) {
        return fail(Errc::invalid_argument, std::format("{}: line {}: {}", kManifest,
                                                        error.source().begin.line, error.description()));
    }
    std::vector<PromptInfo> prompts;
    const toml::array* entries = document["prompt"].as_array();
    if (entries == nullptr) {
        return prompts;  // no prompts yet
    }
    for (std::size_t index = 0; index < entries->size(); ++index) {
        const toml::table* table = (*entries)[index].as_table();
        if (table == nullptr) {
            return fail(Errc::invalid_argument,
                        std::format("{}: 'prompt' must be [[prompt]] tables", kManifest));
        }
        RR_TRY_ASSIGN(PromptInfo info, parse_prompt(*table, index));
        if (std::ranges::any_of(prompts, [&](const PromptInfo& other) { return other.name == info.name; })) {
            return fail(Errc::invalid_argument,
                        std::format("{}: prompt '{}' appears twice", kManifest, info.name));
        }
        prompts.push_back(std::move(info));
    }
    return prompts;
}

using Files = std::map<std::string, std::string>;
using Problems = std::vector<std::string>;

// Every prompt directory has a manifest entry.
void check_directories(const Library& library, const Files& files, Problems& found) {
    std::set<std::string> directories;
    for (const auto& [path, content] : files) {
        if (path.starts_with("prompts/") && path.find('/', 8) != std::string::npos) {
            directories.insert(path.substr(8, path.find('/', 8) - 8));
        }
    }
    for (const std::string& directory : directories) {
        if (library.find(directory) == nullptr) {
            found.push_back(std::format("prompts/{}/ has no [[prompt]] in {}", directory, kManifest));
        }
    }
}

// Versions numbered 1, 2, 3…, each with its own hash, the current one the
// files'. False when the prompt's files are missing.
bool check_versions(const Library& library, const PromptInfo& info, Problems& found) {
    std::set<std::string> hashes;
    for (std::size_t index = 0; index < info.versions.size(); ++index) {
        const Version& version = info.versions[index];
        if (version.number != static_cast<int>(index) + 1) {
            found.push_back(std::format("prompt '{}': version #{} is numbered {}, expected {}", info.name,
                                        index + 1, version.number, index + 1));
        }
        if (!hashes.insert(version.hash).second) {
            found.push_back(
                std::format("prompt '{}': two versions have the hash {}", info.name, version.hash));
        }
    }
    const Result<std::string> hash = library.content_hash(info.name);
    if (!hash.has_value()) {
        found.push_back(hash.error().message());
        return false;
    }
    if (*hash != info.current().hash) {
        found.push_back(
            std::format("prompt '{}' changed since version {}: add [[prompt.version]] number = {}, "
                        "hash = \"{}\", with a note on what changed",
                        info.name, info.current().number, info.current().number + 1, *hash));
    }
    return true;
}

// Both templates parse, and the system template uses only shared variables.
void check_templates(const Files& files, const PromptInfo& info, Problems& found) {
    inja::Environment env = environment();
    const std::string system = system_path(info.name);
    const Result<inja::Template> parsed = parse(env, files.at(system), system);
    if (!parsed.has_value()) {
        found.push_back(parsed.error().message());
    } else {
        VariableCollector collector;
        parsed->root.accept(collector);
        for (const std::string& variable : collector.external()) {
            if (std::ranges::find(info.shared, variable) == info.shared.end()) {
                found.push_back(
                    std::format("prompt '{}': its system part uses '{}', which is not shared: the "
                                "system message must be the same for every order, or no cache can "
                                "serve it. Move it to task.md.inja, or list it in `shared`",
                                info.name, variable));
            }
        }
    }
    const std::string task = task_path(info.name);
    if (const Result<inja::Template> task_parsed = parse(env, files.at(task), task);
        !task_parsed.has_value()) {
        found.push_back(task_parsed.error().message());
    }
}

// The example is there, shaped as documented, and every order of it renders.
void check_example(const Library& library, const Files& files, const PromptInfo& info, Problems& found) {
    const auto example = files.find(example_path(info.name));
    if (example == files.end()) {
        found.push_back(
            std::format("prompt '{}': {} is missing: an example documents the data, and is "
                        "rendered by these checks",
                        info.name, example_path(info.name)));
        return;
    }
    const Result<json::Json> data = json::parse(example->second);
    const bool shaped = data.has_value() && data->is_object() && data->contains("shared") &&
                        data->contains("orders") && (*data)["orders"].is_array() &&
                        (*data)["orders"].size() >= 2;
    if (!shaped) {
        found.push_back(
            std::format("prompt '{}': {} must be {{\"shared\": {{…}}, \"orders\": [two or more]}}", info.name,
                        example_path(info.name)));
        return;
    }
    for (const json::Json& order : (*data)["orders"]) {
        if (const Result<Rendered> rendered = library.render(info.name, (*data)["shared"], order);
            !rendered.has_value()) {
            found.push_back(std::format("prompt '{}': its example does not render: {}", info.name,
                                        rendered.error().message()));
            return;
        }
    }
}

// The answer schema, if any, is there and compiles.
void check_schema(const Files& files, const PromptInfo& info, Problems& found) {
    if (!info.schema.has_value()) {
        return;
    }
    const auto schema = files.find(*info.schema);
    if (schema == files.end()) {
        found.push_back(std::format("prompt '{}': its schema '{}' is missing", info.name, *info.schema));
    } else if (const auto compiled = json::Schema::compile_text(schema->second); !compiled.has_value()) {
        found.push_back(std::format("prompt '{}': its schema '{}' does not compile: {}", info.name,
                                    *info.schema, compiled.error().message()));
    }
}

}  // namespace

const Version& PromptInfo::current() const {
    RR_ASSERT(!versions.empty(), "a prompt always has a version");
    return versions.back();
}

std::string PromptInfo::label() const {
    return std::format("{}@{}", name, current().number);
}

Result<Library> Library::from_files(std::map<std::string, std::string> files) {
    Library library;
    library.files_ = std::move(files);
    const auto manifest = library.files_.find(std::string{kManifest});
    if (manifest != library.files_.end()) {
        RR_TRY_ASSIGN(library.prompts_, parse_manifest(manifest->second));
    }
    return library;
}

Result<Library> Library::embedded() {
    std::map<std::string, std::string> files;
    for (const auto& file : embedded::prompts::files()) {
        files.emplace(std::string{file.path}, std::string{file.content});
    }
    return from_files(std::move(files));
}

Result<Library> Library::from_directory(const std::filesystem::path& root) {
    std::map<std::string, std::string> files;
    for (const std::string_view top : {"prompts", "schemas"}) {
        std::error_code error;
        if (!std::filesystem::is_directory(root / top, error)) {
            continue;
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root / top, error)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::ifstream in(entry.path(), std::ios::binary);
            std::ostringstream content;
            content << in.rdbuf();
            if (!in) {
                return fail(Errc::io, std::format("cannot read '{}'", entry.path().string()));
            }
            files.emplace(std::filesystem::relative(entry.path(), root).generic_string(), content.str());
        }
        if (error) {
            return fail(Errc::io,
                        std::format("cannot list '{}': {}", (root / top).string(), error.message()));
        }
    }
    return from_files(std::move(files));
}

const PromptInfo* Library::find(std::string_view name) const noexcept {
    const auto found = std::ranges::find(prompts_, name, &PromptInfo::name);
    return found == prompts_.end() ? nullptr : &*found;
}

Result<std::string> Library::content_hash(std::string_view name) const {
    const PromptInfo* info = find(name);
    if (info == nullptr) {
        return fail(Errc::not_found, std::format("no prompt '{}' in {}", name, kManifest));
    }
    std::vector<std::string> paths{system_path(name), task_path(name)};
    if (info->schema.has_value()) {
        paths.push_back(*info->schema);
    }
    // Each file as "<path>\n<length>\n<bytes>": no two sets of files hash
    // alike by being cut differently.
    Blake3Hasher hasher;
    for (const std::string& path : paths) {
        const auto file = files_.find(path);
        if (file == files_.end()) {
            return fail(Errc::not_found, std::format("prompt '{}': '{}' is missing", name, path));
        }
        hasher.update(std::format("{}\n{}\n", path, file->second.size()));
        hasher.update(file->second);
    }
    return hasher.finalize().hex();
}

Result<Rendered> Library::render(std::string_view name, const json::Json& shared,
                                 const json::Json& order) const {
    if (find(name) == nullptr) {
        return fail(Errc::not_found, std::format("no prompt '{}' in {}", name, kManifest));
    }
    if (!shared.is_object() || !order.is_object()) {
        return fail(Errc::invalid_argument,
                    std::format("prompt '{}': the shared and the order's data must be JSON objects", name));
    }
    json::Json both = shared;
    for (const auto& [key, value] : order.items()) {
        if (both.contains(key)) {
            return fail(Errc::invalid_argument,
                        std::format("prompt '{}': '{}' is both shared and the order's: which one is meant?",
                                    name, key));
        }
        both[key] = value;
    }
    const auto system = files_.find(system_path(name));
    const auto task = files_.find(task_path(name));
    if (system == files_.end() || task == files_.end()) {
        return fail(Errc::not_found, std::format("prompt '{}': its templates are missing", name));
    }
    inja::Environment env = environment();
    Rendered rendered;
    // The system part sees the shared data and nothing else.
    RR_TRY_ASSIGN(rendered.system, render_template(env, system->second, shared, system->first));
    RR_TRY_ASSIGN(rendered.user, render_template(env, task->second, both, task->first));
    return rendered;
}

std::vector<std::string> Library::problems() const {
    Problems found;
    check_directories(*this, files_, found);
    for (const PromptInfo& info : prompts_) {
        // Without its files, nothing else about a prompt can be checked.
        if (check_versions(*this, info, found)) {
            check_templates(files_, info, found);
            check_example(*this, files_, info, found);
            check_schema(files_, info, found);
        }
    }
    return found;
}

}  // namespace rr::prompt
