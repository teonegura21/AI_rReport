#include <rr/knowledge/artifacts.hpp>

#include <string>
#include <string_view>

#include <rr/core/assert.hpp>
#include <rr/embedded/artifact_schemas.hpp>

namespace rr::knowledge {

const json::SchemaRegistry& artifact_schemas() {
    static const json::SchemaRegistry registry = [] {
        constexpr std::string_view kDirectory = "schemas/";
        constexpr std::string_view kExtension = ".json";
        json::SchemaRegistry schemas;
        for (const embedded::artifact_schemas::File& file : embedded::artifact_schemas::files()) {
            // "schemas/artifacts/atoms.json" is "artifacts/atoms".
            std::string_view name = file.path;
            RR_ASSERT(name.starts_with(kDirectory) && name.ends_with(kExtension),
                      "an artifact schema's path");
            name.remove_prefix(kDirectory.size());
            name.remove_suffix(kExtension.size());
            const Result<void> added = schemas.add_text(std::string{name}, file.content);
            RR_ASSERT(added.has_value(), "a compiled-in artifact schema compiles");
        }
        return schemas;
    }();
    return registry;
}

}  // namespace rr::knowledge
