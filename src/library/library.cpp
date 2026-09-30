#include <rr/library/library.hpp>

#include <cctype>
#include <format>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <rr/core/assert.hpp>
#include <rr/embedded/library_migrations.hpp>

namespace rr::library {

namespace {

// "migrations/0001_accounting.sql" → version 1, named "0001_accounting".
db::Migration migration_from(const embedded::library_migrations::File& file) {
    std::string_view name = file.path;
    name.remove_prefix(name.rfind('/') + 1);  // npos + 1 == 0: no directory
    RR_ASSERT(name.ends_with(".sql"), "migration files end in .sql");
    name.remove_suffix(4);
    RR_ASSERT(name.size() > 5, "migration files are named NNNN_name.sql");
    RR_ASSERT(name[4] == '_', "migration files are named NNNN_name.sql");
    int version = 0;
    for (const char digit : name.substr(0, 4)) {
        RR_ASSERT(std::isdigit(static_cast<unsigned char>(digit)) != 0,
                  "migration files start with four digits");
        version = version * 10 + (digit - '0');
    }
    return db::Migration{.version = version, .name = name, .sql = file.content};
}

}  // namespace

std::span<const db::Migration> migrations() {
    static const std::vector<db::Migration> all = [] {
        std::vector<db::Migration> list;
        for (const auto& file : embedded::library_migrations::files()) {
            list.push_back(migration_from(file));
            RR_ASSERT(std::cmp_equal(list.back().version, list.size()),
                      "migrations are numbered 1, 2, 3, … in order, without gaps");
        }
        return list;
    }();
    return all;
}

Result<db::Database> open_database(const std::filesystem::path& file) {
    if (file.has_parent_path()) {
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        if (error) {
            return fail(Errc::io,
                        std::format("cannot create '{}': {}", file.parent_path().string(), error.message()));
        }
    }
    RR_TRY_ASSIGN(db::Database database, db::Database::open({.path = file}));
    RR_TRY(with_context(database.migrate(migrations()), std::format("while migrating '{}'", file.string())));
    return database;
}

}  // namespace rr::library
