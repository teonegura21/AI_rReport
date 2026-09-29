#include <rr/db/database.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <latch>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/core/scope_exit.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::db {
namespace {

using namespace std::chrono_literals;
using ::testing::HasSubstr;
using testkit::unwrap;

using VideoId = StrongId<struct VideoIdTag>;

Database open_memory() {
    return unwrap(Database::open({.path = ":memory:"}));
}

// items(id, name): the table most tests work with.
void create_items(Database& db) {
    unwrap(db.write([](WriteTx& tx) {
        return tx.execute_script(
            "CREATE TABLE items(id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE) STRICT");
    }));
}

std::int64_t count_items(Database& db) {
    return unwrap(db.read([](ReadTx& tx) { return tx.query_int("SELECT COUNT(*) FROM items"); }));
}

// ---------------------------------------------------------------------------
// Values and statements
// ---------------------------------------------------------------------------
TEST(Database, InMemoryDatabaseStartsEmpty) {
    Database db = open_memory();
    EXPECT_EQ(unwrap(db.schema_version()), 0);
    EXPECT_EQ(db.path(), ":memory:");
}

TEST(Database, ValuesRoundTripThroughEveryStorageClass) {
    Database db = open_memory();
    const Blob bytes{std::byte{0x00}, std::byte{0xFF}, std::byte{0x10}};
    ASSERT_OK(db.write([&](WriteTx& tx) -> Result<void> {
        RR_TRY(tx.execute_script(
            "CREATE TABLE v(i INTEGER, r REAL, t TEXT, b BLOB, n TEXT, et TEXT, eb BLOB, big INTEGER,"
            "               flag INTEGER, id TEXT)"));
        RR_TRY(tx.execute("INSERT INTO v VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)",
                          {-42, 2.5, std::string_view{"h\xC3\xA9llo"}, bytes, std::optional<std::string>{},
                           "", Blob{}, std::uint32_t{4'000'000'000}, true, VideoId{"dQw4w9WgXcQ"}}));
        return ok();
    }));
    int rows = 0;
    ASSERT_OK(db.read([&](ReadTx& tx) {
        return tx.query("SELECT i, r, t, b, n, et, eb, big, flag, id, typeof(eb) FROM v", {},
                        [&](const Row& row) -> Result<void> {
                            ++rows;
                            EXPECT_EQ(row.column_count(), 11);
                            EXPECT_EQ(row.get_int(0), -42);
                            EXPECT_DOUBLE_EQ(row.get_double(1), 2.5);
                            EXPECT_EQ(row.get_text(2), "h\xC3\xA9llo");
                            EXPECT_EQ(row.get_blob(3), bytes);
                            EXPECT_TRUE(row.is_null(4));
                            EXPECT_EQ(row.get_optional_text(4), std::nullopt);
                            EXPECT_EQ(row.get_optional_text(5), std::optional<std::string>{""});  // not NULL
                            EXPECT_FALSE(row.is_null(6));  // an empty blob is not NULL either
                            EXPECT_TRUE(row.get_blob(6).empty());
                            EXPECT_EQ(row.get_text(10), "blob");
                            EXPECT_EQ(row.get_int(7), 4'000'000'000);
                            EXPECT_EQ(row.get_int(8), 1);
                            EXPECT_EQ(row.get_text(9), "dQw4w9WgXcQ");
                            EXPECT_EQ(row.get_optional_double(1), 2.5);
                            EXPECT_EQ(row.get_optional_int(4), std::nullopt);
                            return ok();
                        });
    }));
    EXPECT_EQ(rows, 1);
}

TEST(Database, StatementMistakesAreReportedNotIgnored) {
    Database db = open_memory();
    create_items(db);
    ASSERT_OK(db.write([](WriteTx& tx) -> Result<void> {
        EXPECT_ERRC(tx.execute("INSERT INTO items(name) VALUES (?1)", {}), Errc::invalid_argument);
        EXPECT_ERRC(tx.execute("INSERT INTO items(name) VALUES (?1)", {"a", "b"}), Errc::invalid_argument);
        EXPECT_ERRC(tx.execute("SELECT 1; SELECT 2"),
                    Errc::invalid_argument);  // the C API would drop the 2nd
        EXPECT_ERRC(tx.execute("   "), Errc::invalid_argument);
        EXPECT_ERRC(tx.execute("-- only a comment"), Errc::invalid_argument);
        EXPECT_ERRC(tx.execute("SELEKT 1"), Errc::internal);  // a syntax error is a bug in our SQL
        EXPECT_OK(tx.execute("SELECT 1; -- a trailing comment is fine"));
        return ok();
    }));
}

TEST(Database, ConstraintViolationsAreClassified) {
    Database db = open_memory();
    ASSERT_OK(db.write([](WriteTx& tx) {
        return tx.execute_script(
            "CREATE TABLE parent(id INTEGER PRIMARY KEY) STRICT;"
            "CREATE TABLE child(parent_id INTEGER NOT NULL REFERENCES parent(id)) STRICT;"
            "CREATE TABLE tag(name TEXT NOT NULL UNIQUE) STRICT;");
    }));
    // Clashes with existing data are conflicts...
    const auto duplicate = db.write([](WriteTx& tx) -> Result<void> {
        RR_TRY(tx.execute("INSERT INTO tag VALUES ('ml')"));
        RR_TRY(tx.execute("INSERT INTO tag VALUES ('ml')"));
        return ok();
    });
    EXPECT_ERRC(duplicate, Errc::conflict);
    EXPECT_EQ(unwrap(db.read([](ReadTx& tx) { return tx.query_int("SELECT COUNT(*) FROM tag"); })), 0)
        << "the whole transaction was rolled back, including the first insert";
    EXPECT_ERRC(db.write([](WriteTx& tx) { return tx.execute("INSERT INTO child VALUES (99)"); }),
                Errc::conflict)
        << "foreign keys must be enforced";
    // ...while a value that could never be valid is an invalid argument.
    EXPECT_ERRC(db.write([](WriteTx& tx) { return tx.execute("INSERT INTO parent VALUES (?1)", {"one"}); }),
                Errc::invalid_argument);
    EXPECT_ERRC(db.write([](WriteTx& tx) { return tx.execute("INSERT INTO tag VALUES (NULL)"); }),
                Errc::invalid_argument);
}

TEST(Database, ExecuteCountsOnlyRowsThisStatementChanged) {
    Database db = open_memory();
    create_items(db);
    ASSERT_OK(db.write([](WriteTx& tx) -> Result<void> {
        for (const char* name : {"a", "b", "c"}) {
            EXPECT_EQ(unwrap(tx.execute("INSERT INTO items(name) VALUES (?1)", {name})), 1);
        }
        EXPECT_EQ(tx.last_insert_rowid(), 3);
        EXPECT_EQ(unwrap(tx.execute("UPDATE items SET name = name || '!' WHERE id >= 2")), 2);
        // Not INSERT/UPDATE/DELETE: 0, not the 2 left over from the UPDATE.
        EXPECT_EQ(unwrap(tx.execute("CREATE INDEX items_by_id_name ON items(id, name)")), 0);
        EXPECT_EQ(unwrap(tx.execute("SELECT * FROM items")), 0);
        EXPECT_EQ(unwrap(tx.execute("UPDATE items SET name = 'x' WHERE id > 100")), 0);
        EXPECT_EQ(unwrap(tx.execute("DELETE FROM items")), 3);
        return ok();
    }));
}

TEST(Database, QueryHelpersDistinguishMissingFromNull) {
    Database db = open_memory();
    create_items(db);
    ASSERT_OK(db.read([](ReadTx& tx) -> Result<void> {
        EXPECT_ERRC(tx.query_int("SELECT id FROM items"), Errc::not_found);
        EXPECT_EQ(unwrap(tx.query_optional_int("SELECT id FROM items")), std::nullopt);
        EXPECT_ERRC(tx.query_text("SELECT NULL"), Errc::not_found);
        EXPECT_EQ(unwrap(tx.query_optional_text("SELECT NULL")), std::nullopt);
        EXPECT_EQ(unwrap(tx.query_int("SELECT ?1 + ?2", {40, 2})), 42);
        EXPECT_EQ(unwrap(tx.query_text("SELECT upper(?1)", {"atom"})), "ATOM");
        EXPECT_EQ(unwrap(tx.query_int("SELECT 2 UNION ALL SELECT 1")), 2) << "the first row wins";
        return ok();
    }));
}

TEST(Database, AFailingRowCallbackStopsTheQuery) {
    Database db = open_memory();
    create_items(db);
    ASSERT_OK(db.write(
        [](WriteTx& tx) { return tx.execute("INSERT INTO items(name) VALUES ('a'), ('b'), ('c'), ('d')"); }));
    int seen = 0;
    const auto stopped = db.read([&](ReadTx& tx) {
        return tx.query("SELECT name FROM items ORDER BY id", {}, [&](const Row&) -> Result<void> {
            if (++seen == 2) {
                return fail(Errc::cancelled, "enough");
            }
            return ok();
        });
    });
    EXPECT_ERRC(stopped, Errc::cancelled);
    EXPECT_EQ(seen, 2);
}

TEST(Database, NestedQueriesMayRunTheSameSql) {
    // A recursive walk runs the same SQL at every level of the tree. Each
    // level needs its own statement, or the inner query would reset the outer
    // one halfway through its rows.
    Database db = open_memory();
    ASSERT_OK(db.write([](WriteTx& tx) -> Result<void> {
        RR_TRY(tx.execute_script(
            "CREATE TABLE ideas(id INTEGER PRIMARY KEY, parent_id INTEGER, name TEXT NOT NULL) STRICT"));
        return tx.execute_script(
            "INSERT INTO ideas VALUES (1, NULL, 'thesis'), (2, 1, 'idea A'), (3, 1, 'idea B'),"
            "                         (4, 2, 'claim A1'), (5, 2, 'claim A2'), (6, 3, 'claim B1')");
    }));
    std::vector<std::string> outline;
    ASSERT_OK(db.read([&](ReadTx& tx) {
        std::function<Result<void>(std::int64_t, std::size_t)> walk = [&](std::int64_t parent,
                                                                          std::size_t depth) -> Result<void> {
            return tx.query("SELECT id, name FROM ideas WHERE parent_id = ?1 ORDER BY id", {parent},
                            [&](const Row& row) -> Result<void> {
                                if (outline.size() > 100) {
                                    return fail(Errc::internal, "runaway walk");
                                }
                                outline.push_back(std::string(depth * 2, ' ') + row.get_text(1));
                                return walk(row.get_int(0), depth + 1);
                            });
        };
        return walk(1, 0);
    }));
    EXPECT_EQ(outline,
              (std::vector<std::string>{"idea A", "  claim A1", "  claim A2", "idea B", "  claim B1"}));
}

TEST(Database, DynamicSqlBeyondTheStatementCacheStillRuns) {
    Database db = open_memory();
    ASSERT_OK(db.read([](ReadTx& tx) -> Result<void> {
        for (int index = 0; index < 700; ++index) {  // more distinct statements than the cache holds
            RR_TRY_ASSIGN(const std::int64_t value, tx.query_int(std::format("SELECT {}", index)));
            EXPECT_EQ(value, index);
        }
        return ok();
    }));
}

// ---------------------------------------------------------------------------
// Transactions
// ---------------------------------------------------------------------------
TEST(Database, AFailedWriteRollsBackEveryStatement) {
    Database db = open_memory();
    create_items(db);
    const auto abandoned = db.write([](WriteTx& tx) -> Result<void> {
        RR_TRY(tx.execute("INSERT INTO items(name) VALUES ('a')"));
        RR_TRY(tx.execute("INSERT INTO items(name) VALUES ('b')"));
        return fail(Errc::refused, "changed my mind");
    });
    EXPECT_ERRC(abandoned, Errc::refused);
    EXPECT_EQ(count_items(db), 0);
}

TEST(Database, AnExceptionRollsBackAndReleasesTheWriter) {
    Database db = open_memory();
    create_items(db);
    EXPECT_THROW((void)db.write([](WriteTx& tx) -> Result<void> {
        RR_TRY(tx.execute("INSERT INTO items(name) VALUES ('ghost')"));
        throw std::runtime_error("bug in the callback");
    }),
                 std::runtime_error);
    EXPECT_EQ(count_items(db), 0);
    // The writer mutex was released and the connection left the transaction.
    ASSERT_OK(db.write([](WriteTx& tx) { return tx.execute("INSERT INTO items(name) VALUES ('real')"); }));
    EXPECT_EQ(count_items(db), 1);
}

TEST(Database, ReadTransactionsCannotWrite) {
    // In memory, reads share the writer connection: the strictest case.
    Database db = open_memory();
    create_items(db);
    EXPECT_ERRC(
        db.read([](ReadTx& tx) { return tx.query("INSERT INTO items(name) VALUES ('sneaky')", {}, {}); }),
        Errc::invalid_argument);
    EXPECT_EQ(count_items(db), 0);
}

TEST(Database, NestedTransactionsOnOneDatabaseFailInsteadOfDeadlocking) {
    Database db = open_memory();
    ASSERT_OK(db.write([&](WriteTx&) -> Result<void> {
        EXPECT_ERRC(db.read([](ReadTx& tx) { return tx.query_int("SELECT 1"); }), Errc::internal);
        EXPECT_ERRC(db.write([](WriteTx& tx) { return tx.query_int("SELECT 1"); }), Errc::internal);
        return ok();
    }));
    ASSERT_OK(db.read([&](ReadTx&) -> Result<void> {
        EXPECT_ERRC(db.read([](ReadTx& tx) { return tx.query_int("SELECT 1"); }), Errc::internal);
        return ok();
    }));
    // The refused attempts left nothing behind...
    EXPECT_EQ(unwrap(db.read([](ReadTx& tx) { return tx.query_int("SELECT 7"); })), 7);
    // ...and transactions on *different* databases may nest.
    Database other = open_memory();
    EXPECT_EQ(unwrap(db.read(
                  [&](ReadTx&) { return other.read([](ReadTx& tx) { return tx.query_int("SELECT 8"); }); })),
              8);
}

TEST(Database, CanBeMoved) {
    Database db = open_memory();
    create_items(db);
    Database moved = std::move(db);
    EXPECT_EQ(count_items(moved), 0);
}

TEST(Database, Fts5IsAvailableForFullTextSearch) {
    Database db = open_memory();
    ASSERT_TRUE(unwrap(db.supports_fts5())) << "rReport's library search needs SQLite built with FTS5";
    ASSERT_OK(db.write([](WriteTx& tx) -> Result<void> {
        RR_TRY(tx.execute_script("CREATE VIRTUAL TABLE chunks_fts USING fts5(text)"));
        RR_TRY(tx.execute("INSERT INTO chunks_fts(rowid, text) VALUES (1, ?1), (2, ?2)",
                          {"data curation beats model size", "a joke about GPUs"}));
        return ok();
    }));
    EXPECT_EQ(unwrap(db.read([](ReadTx& tx) {
                  return tx.query_int("SELECT rowid FROM chunks_fts WHERE chunks_fts MATCH ?1", {"curation"});
              })),
              1);
}

// ---------------------------------------------------------------------------
// Migrations
// ---------------------------------------------------------------------------
constexpr std::array kMigrations{
    Migration{.version = 1,
              .name = "0001_items",
              .sql = "CREATE TABLE items(id INTEGER PRIMARY KEY, name TEXT NOT NULL) STRICT;"},
    Migration{
        .version = 2,
        .name = "0002_tags",
        .sql = "CREATE TABLE tags(item_id INTEGER NOT NULL REFERENCES items(id), tag TEXT NOT NULL) STRICT;"
               "CREATE INDEX tags_by_item ON tags(item_id);"},
};

std::int64_t count_schema_objects(Database& db) {
    return unwrap(db.read([](ReadTx& tx) {
        return tx.query_int("SELECT COUNT(*) FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%'");
    }));
}

TEST(Migrations, ApplyInOrderOnceThenOnlyNewOnes) {
    Database db = open_memory();
    EXPECT_EQ(unwrap(db.migrate(kMigrations)), 2);
    EXPECT_EQ(unwrap(db.schema_version()), 2);
    EXPECT_EQ(count_schema_objects(db), 3);  // items, tags, tags_by_item

    EXPECT_EQ(unwrap(db.migrate(kMigrations)), 2) << "re-running applies nothing";

    const std::array extended{
        kMigrations[0], kMigrations[1],
        Migration{.version = 3, .name = "0003_notes", .sql = "CREATE TABLE notes(t TEXT)"}};
    EXPECT_EQ(unwrap(db.migrate(extended)), 3);
    EXPECT_EQ(count_schema_objects(db), 4);
}

TEST(Migrations, AFailingMigrationRollsBackCompletely) {
    Database db = open_memory();
    const std::array migrations{
        kMigrations[0],
        Migration{
            .version = 2, .name = "0002_broken", .sql = "CREATE TABLE half(x INTEGER); THIS IS NOT SQL;"},
    };
    const auto result = db.migrate(migrations);
    ASSERT_FALSE(result.has_value());
    EXPECT_THAT(result.error().describe(), HasSubstr("while applying migration 2 (0002_broken)"));
    EXPECT_EQ(unwrap(db.schema_version()), 1) << "migration 1 stays applied";
    EXPECT_EQ(unwrap(db.read([](ReadTx& tx) {
                  return tx.query_int("SELECT COUNT(*) FROM sqlite_schema WHERE name = 'half'");
              })),
              0)
        << "SQLite DDL is transactional: the half-created table is gone";
}

TEST(Migrations, RefuseADatabaseNewerThanTheProgram) {
    Database db = open_memory();
    ASSERT_OK(db.migrate(kMigrations));
    EXPECT_ERRC(db.migrate(std::span{kMigrations}.first(1)), Errc::conflict);
}

TEST(Migrations, RejectGapsBeforeRunningAnything) {
    Database db = open_memory();
    const std::array gap{kMigrations[0], Migration{.version = 3, .name = "0003", .sql = "SELECT 1"}};
    EXPECT_ERRC(db.migrate(gap), Errc::internal);
    const std::array late_start{Migration{.version = 2, .name = "0002", .sql = "SELECT 1"}};
    EXPECT_ERRC(db.migrate(late_start), Errc::internal);
    EXPECT_EQ(unwrap(db.schema_version()), 0);
}

TEST(Migrations, AScriptMustNotEndTheTransaction) {
    Database db = open_memory();
    const std::array sneaky{
        Migration{.version = 1, .name = "0001", .sql = "CREATE TABLE t(x INTEGER); COMMIT;"}};
    EXPECT_ERRC(db.migrate(sneaky), Errc::internal);
    EXPECT_EQ(unwrap(db.schema_version()), 0);
}

// ---------------------------------------------------------------------------
// File databases: WAL, persistence, concurrency
// ---------------------------------------------------------------------------
TEST(FileDatabase, UsesWalPersistsDataAndClosesCleanly) {
    const testkit::TempDir dir{"rr_db_test"};
    const auto path = dir / "library.db";
    {
        Database db = unwrap(Database::open({.path = path}));
        create_items(db);
        ASSERT_OK(db.write([](WriteTx& tx) -> Result<void> {
            EXPECT_EQ(unwrap(tx.query_text("PRAGMA journal_mode")), "wal");
            return tx.execute_script("INSERT INTO items(name) VALUES ('kept')");
        }));
        // Reading through the pool makes the readers open the WAL too, which
        // is what makes the close order below matter.
        EXPECT_EQ(count_items(db), 1);
        EXPECT_TRUE(std::filesystem::exists(dir / "library.db-wal"));
    }
    // The writer closes last, so it can checkpoint the WAL and remove it. A
    // read-only connection closing last cannot, and would leave it behind.
    EXPECT_FALSE(std::filesystem::exists(dir / "library.db-wal"));

    Database reopened = unwrap(Database::open({.path = path}));
    EXPECT_EQ(unwrap(reopened.read([](ReadTx& tx) { return tx.query_text("SELECT name FROM items"); })),
              "kept");
}

TEST(FileDatabase, OpenFailuresAreClassified) {
    const testkit::TempDir dir{"rr_db_test"};
    EXPECT_ERRC(Database::open({.path = dir / "missing" / "library.db"}), Errc::io);
    const auto garbage = dir.write_file("garbage.db", std::string(4096, 'x'));
    EXPECT_ERRC(Database::open({.path = garbage}), Errc::integrity);
}

TEST(FileDatabase, AReadTransactionKeepsItsSnapshotWhileTheWriterCommits) {
    const testkit::TempDir dir{"rr_db_test"};
    // A short busy timeout turns a regression (the reader blocking the
    // writer) into a failure instead of a slow test.
    Database db = unwrap(Database::open({.path = dir / "library.db", .busy_timeout = 200ms}));
    create_items(db);

    std::latch snapshot_taken{1};
    std::latch write_finished{1};
    Result<std::pair<std::int64_t, std::int64_t>> counts = fail(Errc::internal, "the reader did not run");
    std::jthread reader([&] {
        bool signalled = false;
        const ScopeExit unblock_main{[&] {
            if (!signalled) {
                snapshot_taken.count_down();
            }
        }};
        counts = db.read([&](ReadTx& tx) -> Result<std::pair<std::int64_t, std::int64_t>> {
            RR_TRY_ASSIGN(const std::int64_t before, tx.query_int("SELECT COUNT(*) FROM items"));
            signalled = true;
            snapshot_taken.count_down();
            write_finished.wait();
            RR_TRY_ASSIGN(const std::int64_t after, tx.query_int("SELECT COUNT(*) FROM items"));
            return std::pair{before, after};
        });
    });
    snapshot_taken.wait();
    const auto written =
        db.write([](WriteTx& tx) { return tx.execute("INSERT INTO items(name) VALUES ('new')"); });
    write_finished.count_down();
    reader.join();

    ASSERT_OK(written) << "an open read transaction must not block the writer";
    ASSERT_OK(counts);
    EXPECT_EQ(counts->first, 0);
    EXPECT_EQ(counts->second, 0) << "one read transaction sees one snapshot";
    EXPECT_EQ(count_items(db), 1) << "the next read transaction sees the commit";
}

TEST(FileDatabase, ConcurrentWritersAreSerializedAndReadersSeeOnlyCommittedValues) {
    const testkit::TempDir dir{"rr_db_test"};
    Database db = unwrap(Database::open({.path = dir / "library.db", .reader_count = 2}));
    ASSERT_OK(db.write([](WriteTx& tx) {
        return tx.execute_script(
            "CREATE TABLE counter(value INTEGER NOT NULL) STRICT; INSERT INTO counter VALUES (0);");
    }));

    constexpr int kWriters = 6;
    constexpr int kReaders = 6;  // more readers than pooled connections: some must wait
    constexpr int kRounds = 40;
    std::atomic<int> failures{0};
    {
        std::vector<std::jthread> threads;
        threads.reserve(kWriters + kReaders);
        for (int writer = 0; writer < kWriters; ++writer) {
            threads.emplace_back([&] {
                for (int round = 0; round < kRounds; ++round) {
                    // Read-modify-write: correct only if writes are serialized.
                    const auto incremented = db.write([](WriteTx& tx) -> Result<void> {
                        RR_TRY_ASSIGN(const std::int64_t value, tx.query_int("SELECT value FROM counter"));
                        RR_TRY(tx.execute("UPDATE counter SET value = ?1", {value + 1}));
                        return ok();
                    });
                    if (!incremented.has_value()) {
                        ++failures;
                    }
                }
            });
        }
        for (int reader = 0; reader < kReaders; ++reader) {
            threads.emplace_back([&] {
                std::int64_t last = 0;
                for (int round = 0; round < kRounds; ++round) {
                    const auto value =
                        db.read([](ReadTx& tx) { return tx.query_int("SELECT value FROM counter"); });
                    if (!value.has_value() || *value < last) {
                        ++failures;  // committed values only ever grow
                    } else {
                        last = *value;
                    }
                }
            });
        }
    }  // the jthreads join here
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(unwrap(db.read([](ReadTx& tx) { return tx.query_int("SELECT value FROM counter"); })),
              kWriters * kRounds);
}

}  // namespace
}  // namespace rr::db
