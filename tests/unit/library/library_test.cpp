#include <rr/library/library.hpp>

#include <filesystem>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::library {
namespace {

using ::testing::HasSubstr;
using testkit::unwrap;

TEST(Library, MigrationsAreNumberedInOrder) {
    const auto all = migrations();
    ASSERT_FALSE(all.empty());
    for (std::size_t index = 0; index < all.size(); ++index) {
        EXPECT_EQ(all[index].version, static_cast<int>(index) + 1);
        EXPECT_FALSE(all[index].sql.empty());
    }
    EXPECT_EQ(all[0].name, "0001_accounting");
    EXPECT_THAT(all[0].sql, HasSubstr("CREATE TABLE llm_calls"));
    ASSERT_GE(all.size(), 2U);
    EXPECT_EQ(all[1].name, "0002_call_memo");
    EXPECT_THAT(all[1].sql, HasSubstr("CREATE TABLE llm_memo"));
}

TEST(Library, OpeningCreatesTheDatabaseWithTheSchema) {
    const testkit::TempDir dir;
    const std::filesystem::path file = dir / "libraries" / "default" / "library.db";
    {
        db::Database database = unwrap(open_database(file));
        EXPECT_EQ(unwrap(database.schema_version()), static_cast<int>(migrations().size()));
        EXPECT_EQ(unwrap(database.read([](db::ReadTx& tx) {
                      return tx.query_int("SELECT count(*) FROM sqlite_master WHERE name = 'llm_calls'");
                  })),
                  1);
    }
    db::Database again = unwrap(open_database(file));
    EXPECT_EQ(unwrap(again.schema_version()), static_cast<int>(migrations().size()))
        << "reopening is harmless";
}

TEST(Library, RefusesADatabaseFromANewerRreport) {
    const testkit::TempDir dir;
    const std::filesystem::path file = dir / "library.db";
    {
        db::Database database = unwrap(open_database(file));
        ASSERT_OK(
            database.write([](db::WriteTx& tx) { return tx.execute_script("PRAGMA user_version = 999"); }));
    }
    const auto newer = open_database(file);
    ASSERT_FALSE(newer.has_value());
    EXPECT_THAT(newer.error().describe(), HasSubstr("while migrating"));
}

}  // namespace
}  // namespace rr::library
