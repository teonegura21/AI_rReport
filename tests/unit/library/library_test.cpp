#include <rr/library/library.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

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
    ASSERT_GE(all.size(), 3U);
    EXPECT_EQ(all[2].name, "0003_missions");
    EXPECT_THAT(all[2].sql, HasSubstr("CREATE TABLE missions"));
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

// Migration 0007 makes `jobs` anew, the first to rebuild a table others refer
// to (rr/db/database.hpp): a library of version 6, with a job of each kind
// and rows that refer to them, upgraded.
TEST(Library, JobsMadeAnewByMigration7KeepEveryJobAndWhatRefersToThem) {
    db::Database database = unwrap(db::Database::open({.path = ":memory:"}));
    ASSERT_EQ(unwrap(database.migrate(migrations().first(6))), 6);
    unwrap(database.write([](db::WriteTx& tx) -> Result<void> {
        RR_TRY(tx.execute_script(
            "INSERT INTO jobs (id, source, tier, salt, status, error, title, elapsed_ms, created_at_ms, "
            "updated_at_ms, kind) VALUES "
            "('A', 'talk.vtt', 'max', 1, 'done', NULL, 'Weighing radium', 61000, 1, 2, 'article'), "
            "('N', 'eval/data/x/x.source.json', 'max', 0, 'failed', 'no pod', NULL, 0, 3, 4, "
            "'annotation');"
            "INSERT INTO artifacts (hash, schema, codec, body, bytes, created_at_ms) "
            "VALUES ('h', 'artifacts/source', 'json', CAST('{}' AS BLOB), 2, 1);"
            "INSERT INTO stage_runs (job_id, stage, key, status, cached, artifact, started_at_ms, "
            "finished_at_ms) VALUES ('A', 'source', 'k', 'done', 0, 'h', 1, 2);"
            "INSERT INTO eval_runs (id, label, eval_set, split, tier, replicates, created_at_ms) "
            "VALUES ('R', 'base', 'm2', 'dev', 'max', 2, 1);"
            "INSERT INTO eval_jobs (run_id, video, replicate, job_id) VALUES ('R', 'talk', 1, 'A');"));
        return ok();
    }));

    ASSERT_EQ(unwrap(database.migrate(migrations())), static_cast<int>(migrations().size()));
    EXPECT_EQ(
        unwrap(database.read([](db::ReadTx& tx) {
            return tx.query_text(
                "SELECT group_concat(id || ' ' || source || ' ' || tier || ' ' || salt || ' ' || status || "
                "' ' || coalesce(error, '-') || ' ' || coalesce(title, '-') || ' ' || elapsed_ms || ' ' || "
                "created_at_ms || ' ' || updated_at_ms || ' ' || kind, '; ') FROM jobs");
        })),
        "A talk.vtt max 1 done - Weighing radium 61000 1 2 article; "
        "N eval/data/x/x.source.json max 0 failed no pod - 0 3 4 annotation")
        << "every job, every column";
    EXPECT_EQ(unwrap(database.read([](db::ReadTx& tx) {
                  return tx.query_int(
                      "SELECT COUNT(*) FROM stage_runs JOIN eval_jobs USING (job_id) JOIN jobs ON jobs.id = "
                      "job_id");
              })),
              1)
        << "what referred to the job refers to it still";
    unwrap(database.write([](db::WriteTx& tx) -> Result<void> {
        RR_TRY(tx.execute(
            "INSERT INTO jobs (id, source, tier, salt, status, elapsed_ms, created_at_ms, "
            "updated_at_ms, kind) VALUES ('J', 'A', 'eval-v1', 0, 'running', 0, 5, 5, 'judgement')"));
        RR_TRY(tx.execute(
            "INSERT INTO eval_judgements (article_job_id, panel, job_id) VALUES ('A', 'eval-v1', 'J')"));
        return ok();
    }));
    const Result<void> dangling = database.write([](db::WriteTx& tx) -> Result<void> {
        RR_TRY(tx.execute(
            "INSERT INTO eval_jobs (run_id, video, replicate, job_id) VALUES ('R', 'talk', 2, 'Z')"));
        return ok();
    });
    EXPECT_ERRC(dangling, Errc::conflict) << "a job of no row refused: foreign keys on again";
}

// Migration 0008 gives eval runs their first replicate: every run made
// before starts at 0, as it did.
TEST(Library, Migration8StartsEveryRunMadeBeforeAtReplicateZero) {
    db::Database database = unwrap(db::Database::open({.path = ":memory:"}));
    ASSERT_EQ(unwrap(database.migrate(migrations().first(7))), 7);
    unwrap(database.write([](db::WriteTx& tx) -> Result<void> {
        RR_TRY(
            tx.execute("INSERT INTO eval_runs (id, label, eval_set, split, tier, replicates, created_at_ms) "
                       "VALUES ('R', 'base', 'm2', 'dev', 'max', 3, 1)"));
        return ok();
    }));
    ASSERT_GE(migrations().size(), 8U);
    ASSERT_EQ(unwrap(database.migrate(migrations().first(8))), 8);
    EXPECT_EQ(unwrap(database.read([](db::ReadTx& tx) {
                  return tx.query_int("SELECT first_replicate FROM eval_runs WHERE id = 'R'");
              })),
              0);
    const Result<void> negative = database.write([](db::WriteTx& tx) -> Result<void> {
        RR_TRY(
            tx.execute("INSERT INTO eval_runs (id, label, eval_set, split, tier, replicates, created_at_ms, "
                       "first_replicate) VALUES ('S', 'aa', 'm2', 'dev', 'max', 3, 1, -1)"));
        return ok();
    });
    EXPECT_ERRC(negative, Errc::invalid_argument) << "the table's own check";
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
