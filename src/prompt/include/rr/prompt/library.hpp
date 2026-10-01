// rr/prompt/library.hpp — the prompts: their text, their versions, and how
// they are rendered so that an engine's prompt cache can serve them.
//
// Why this exists
//   Every mission the army sends (rr_agents) is a prompt rendered with a
//   packet. Prompts are the product's most important source code, and they
//   are treated as such:
//     * Each prompt is two files, prompts/<name>/system.md.inja and
//       task.md.inja, templates in inja's syntax (Jinja's subset), with an
//       example.json of the data they are rendered with. The example is the
//       prompt's documentation, and the checks below render it.
//     * prompts/manifest.toml names each prompt, says what it is for, which
//       answer schema it uses, how it is known to fail, and keeps its
//       history: a [[prompt.version]] per change, with the content hash of
//       that version. A prompt whose text (or schema) changed without a new
//       version is a failing check, so every change is numbered, and the
//       evaluation can compare "atomize v4" with "atomize v5".
//     * The prompts are compiled into the binary; a directory can stand in
//       for them while iterating (Library::from_directory()).
//
//   **The cache-aware layout, enforced.** llama-server reuses a prompt's
//   longest common prefix with what a slot read before; on hybrid models
//   (Qwen3.8) only from checkpoints at the start of the last user message.
//   So everything a stage's orders share goes in the system message, and
//   only the order's own packet and task go in the last user message:
//     * the system template may use only the variables the manifest lists as
//       `shared` (the role, the rules, the video's brief, the glossary).
//       This is checked statically, on the template's syntax tree, so a
//       branch the example never takes is checked too;
//     * rendering gives the system template the shared data and nothing
//       else, so the system message is the same for every order by
//       construction: a slip can only fail loudly, never leak an order's
//       data into the shared prefix.
//
//   **Strict rendering.** A variable the data does not define is an error,
//   not an empty string: a prompt missing its packet would otherwise ask the
//   model about nothing, and the answer would look fine.
//
// Thread-safety: a Library is immutable once loaded and safe to share.
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/json/json.hpp>

namespace rr::prompt {

// One numbered version of a prompt, as the manifest keeps it.
struct Version {
    int number = 0;
    std::string hash{};  // BLAKE3 of the prompt's files at that version (content_hash())
    std::string note{};  // what changed, for the history

    friend bool operator==(const Version&, const Version&) = default;
};

// A prompt's manifest entry.
struct PromptInfo {
    std::string name{};
    std::string purpose{};
    std::optional<std::string> schema{};       // path of its answer schema, from the project root
    std::vector<std::string> shared{};         // the variables its system part may use
    std::vector<std::string> failure_modes{};  // how it is known to go wrong, for reviewers
    std::vector<Version> versions{};           // oldest first; the last is the current one

    [[nodiscard]] const Version& current() const;  // the last version; there is always one
    // "atomize@4": what results and reports name.
    [[nodiscard]] std::string label() const;

    friend bool operator==(const PromptInfo&, const PromptInfo&) = default;
};

// A rendered prompt: the system message, the same for every order of a
// stage, and the order's last user message.
struct Rendered {
    std::string system{};
    std::string user{};

    friend bool operator==(const Rendered&, const Rendered&) = default;
};

class Library {
public:
    // The prompts compiled into this binary.
    [[nodiscard]] static Result<Library> embedded();
    // The prompts in `root` (a copy of the project's layout: root/prompts/…,
    // and the schemas the manifest names, root/schemas/…).
    [[nodiscard]] static Result<Library> from_directory(const std::filesystem::path& root);
    // From files given by path (relative to the project root) and content.
    [[nodiscard]] static Result<Library> from_files(std::map<std::string, std::string> files);

    [[nodiscard]] const std::vector<PromptInfo>& prompts() const noexcept { return prompts_; }
    [[nodiscard]] const PromptInfo* find(std::string_view name) const noexcept;

    // Renders prompt `name`: its system part with `shared` alone, its task
    // part with `shared` and `order` together. Both must be JSON objects, and
    // no key may be in both. Errc::not_found for an unknown prompt;
    // Errc::invalid_argument for a variable the data does not define, or a
    // template error.
    [[nodiscard]] Result<Rendered> render(std::string_view name, const json::Json& shared,
                                          const json::Json& order) const;

    // The content hash of prompt `name`'s files as they are now: its two
    // templates, and its schema if it has one.
    [[nodiscard]] Result<std::string> content_hash(std::string_view name) const;

    // Everything wrong with the library, one line each; empty when nothing is
    // (the L0 suite requires that):
    //   * a prompt whose files are missing, or a prompt directory without a
    //     manifest entry;
    //   * versions not numbered 1, 2, 3…, or two versions with one hash;
    //   * a current version whose hash is not the files' (the line says the
    //     next version's number and hash);
    //   * a template that does not parse;
    //   * a system template using a variable not listed as shared;
    //   * an example that is missing, or one of whose orders does not render;
    //   * a schema that is missing or does not compile.
    [[nodiscard]] std::vector<std::string> problems() const;

private:
    Library() = default;

    std::vector<PromptInfo> prompts_;
    std::map<std::string, std::string> files_;  // path from the project root -> content
};

}  // namespace rr::prompt
