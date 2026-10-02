// rr/db/database.hpp — SQLite access without exceptions, with scoped transactions.
//
// Why this exists
//   Each library is one SQLite file (ADR-0008). This module is the only code
//   that talks to the SQLite C API. It provides:
//
//   * Scoped transactions. Work happens inside a callback, and the callback's
//     Result decides the outcome: success commits; failure or an exception
//     rolls back. A half-applied write cannot even be expressed:
//
//       db.write([&](WriteTx& tx) -> Result<void> {
//           RR_TRY(tx.execute("INSERT INTO jobs(id, kind) VALUES (?1, ?2)", {job_id, "ingest"}));
//           RR_TRY(tx.execute("UPDATE videos SET status = 'queued' WHERE id = ?1", {video_id}));
//           return ok();
//       });
//
//   * Concurrency matched to SQLite's WAL mode. One writer connection,
//     serialized by a mutex, starts every write with BEGIN IMMEDIATE, so the
//     write lock is taken up front instead of failing halfway through. Reads
//     use a small pool of read-only connections; each read transaction sees
//     one consistent snapshot, and readers never block the writer or each
//     other.
//
//   * Migrations: numbered SQL scripts applied in order, each in its own
//     transaction, tracked with PRAGMA user_version. A database newer than the
//     program is refused rather than silently misread. Foreign keys are off
//     while a migration runs, so that it may rebuild a table others refer to
//     (SQLite's way to change what ALTER TABLE cannot, such as a CHECK), and
//     every reference is checked before it commits.
//
// Invariants
//   * A connection is returned to its pool in autocommit mode (no transaction
//     open), whatever happened inside the callback.
//   * A thread holds at most one transaction per Database. Nesting read() or
//     write() on the *same* database would deadlock (the writer mutex is not
//     recursive; one thread can exhaust a small reader pool) or silently read
//     a snapshot that misses the enclosing write. It fails with
//     Errc::internal instead: use the ReadTx/WriteTx you were given.
//     Transactions on *different* databases may nest.
//   * Statements run through ReadTx are checked to be read-only, so a read()
//     callback cannot write — even on an in-memory database, whose reads
//     share the writer connection.
//
// Thread-safety: Database is thread-safe. ReadTx, WriteTx and Row objects are
//   valid only inside the callback that received them, on that thread. A
//   Database must not be destroyed or moved while transactions are running.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/db/value.hpp>

namespace rr::db {

struct Connection;  // an open sqlite3 handle plus its statement cache (defined in database.cpp)

// One result row, valid only during the row callback that received it.
// Getters follow SQLite's conversion rules (e.g. get_int on NULL yields 0);
// use the get_optional_* variants for nullable columns.
class Row {
public:
    explicit Row(void* statement) noexcept : statement_(statement) {}

    [[nodiscard]] int column_count() const noexcept;
    [[nodiscard]] bool is_null(int column) const noexcept;
    [[nodiscard]] std::int64_t get_int(int column) const noexcept;
    [[nodiscard]] double get_double(int column) const noexcept;
    [[nodiscard]] std::string get_text(int column) const;
    [[nodiscard]] Blob get_blob(int column) const;
    [[nodiscard]] std::optional<std::int64_t> get_optional_int(int column) const noexcept;
    [[nodiscard]] std::optional<double> get_optional_double(int column) const noexcept;
    [[nodiscard]] std::optional<std::string> get_optional_text(int column) const;

private:
    void* statement_;  // sqlite3_stmt*, kept opaque so this header does not include sqlite3.h
};

using RowCallback = std::function<Result<void>(const Row&)>;

// The view of the database inside Database::read(): read-only statements.
class ReadTx {
public:
    ReadTx(const ReadTx&) = delete;
    ReadTx& operator=(const ReadTx&) = delete;
    ReadTx(ReadTx&&) = delete;
    ReadTx& operator=(ReadTx&&) = delete;
    ~ReadTx() = default;

    // Runs one statement (parameters ?1..?N) and calls `on_row` for every row.
    // A failing callback stops the query and its error is returned unchanged.
    // Queries may nest: a row callback can run further queries, including the
    // same SQL (e.g. a recursive walk over a tree).
    [[nodiscard]] Result<void> query(std::string_view sql, std::span<const Value> params,
                                     const RowCallback& on_row);
    [[nodiscard]] Result<void> query(std::string_view sql, std::initializer_list<Value> params,
                                     const RowCallback& on_row) {
        return query(sql, std::span<const Value>{params.begin(), params.size()}, on_row);
    }

    // Single-value helpers read column 0 of the first row.
    // The query_* forms fail with Errc::not_found when there is no row or the
    // value is NULL; the query_optional_* forms return std::nullopt instead.
    [[nodiscard]] Result<std::int64_t> query_int(std::string_view sql,
                                                 std::initializer_list<Value> params = {});
    [[nodiscard]] Result<std::string> query_text(std::string_view sql,
                                                 std::initializer_list<Value> params = {});
    [[nodiscard]] Result<std::optional<std::int64_t>> query_optional_int(
        std::string_view sql, std::initializer_list<Value> params = {});
    [[nodiscard]] Result<std::optional<std::string>> query_optional_text(
        std::string_view sql, std::initializer_list<Value> params = {});

protected:
    ReadTx(Connection& connection, bool writable) noexcept : connection_(&connection), writable_(writable) {}

    [[nodiscard]] Connection& connection() const noexcept { return *connection_; }

private:
    friend class Database;
    explicit ReadTx(Connection& connection) noexcept : ReadTx(connection, false) {}

    Connection* connection_;
    bool writable_;  // false: statements that would modify the database are rejected
};

// The view of the database inside Database::write(): everything ReadTx can
// do, plus statements that modify the database.
class WriteTx : public ReadTx {
public:
    // Runs one statement. Returns how many rows it inserted, updated or
    // deleted (not counting trigger side effects); 0 for other statements.
    [[nodiscard]] Result<std::int64_t> execute(std::string_view sql, std::span<const Value> params);
    [[nodiscard]] Result<std::int64_t> execute(std::string_view sql,
                                               std::initializer_list<Value> params = {}) {
        return execute(sql, std::span<const Value>{params.begin(), params.size()});
    }

    // Runs ';'-separated statements without parameters (schema scripts). A
    // script must not end the enclosing transaction (BEGIN/COMMIT/ROLLBACK);
    // that is detected and reported as Errc::internal.
    [[nodiscard]] Result<void> execute_script(std::string_view sql);

    // The rowid of the most recent successful INSERT on this connection.
    [[nodiscard]] std::int64_t last_insert_rowid() const noexcept;

private:
    friend class Database;
    explicit WriteTx(Connection& connection) noexcept : ReadTx(connection, true) {}
};

struct Migration {
    int version;            // 1, 2, 3, … without gaps
    std::string_view name;  // e.g. "0001_init"
    std::string_view sql;   // the statements; no BEGIN/COMMIT (each migration is one transaction)
};

class Database {
public:
    struct Options {
        std::filesystem::path path;                    // ":memory:" for a private in-memory database (tests)
        std::size_t reader_count = 4;                  // pooled read-only connections (file databases)
        std::chrono::milliseconds busy_timeout{5000};  // wait this long for another process's lock
    };

    // Opens (creating if needed) the database in WAL mode. Fails with
    // Errc::unsupported when WAL is unavailable (e.g. a network filesystem).
    [[nodiscard]] static Result<Database> open(const Options& options);

    Database(Database&&) noexcept;
    Database& operator=(Database&&) noexcept;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    ~Database();

    // Runs `fn(ReadTx&)` in a read transaction on a pooled connection and
    // returns its result. `fn` must return a Result<T>.
    template <class F>
    auto read(F&& fn) -> std::invoke_result_t<F, ReadTx&>;

    // Runs `fn(WriteTx&)` in a BEGIN IMMEDIATE transaction on the writer
    // connection. Commits when `fn` succeeds; rolls back when it fails or
    // throws. `fn` must return a Result<T>.
    template <class F>
    auto write(F&& fn) -> std::invoke_result_t<F, WriteTx&>;

    // Applies the pending migrations and returns the resulting schema version.
    // Each runs with foreign keys off, and commits only when every reference
    // still holds: one that breaks a reference fails with Errc::integrity
    // and is rolled back. Foreign keys are on again after each, whatever
    // happened.
    [[nodiscard]] Result<int> migrate(std::span<const Migration> migrations);

    [[nodiscard]] Result<int> schema_version();
    [[nodiscard]] Result<bool> supports_fts5();
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    struct Impl;

    // A connection checked out for one transaction. Leases held by a thread
    // form an intrusive stack (innermost first) through `outer_`, which is how
    // nested transactions are detected without any allocation. Immovable: a
    // Lease lives in exactly one stack frame, from checkout to release.
    class Lease {
    public:
        Lease(Impl& owner, Connection& connection, bool writer) noexcept;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&&) = delete;
        Lease& operator=(Lease&&) = delete;
        // Rolls back any transaction still open, then returns the connection.
        ~Lease();

        [[nodiscard]] Connection& connection() const noexcept { return *connection_; }
        [[nodiscard]] static bool held_by_this_thread(const Impl& owner) noexcept;

    private:
        static const Lease*& innermost() noexcept;  // this thread's stack top

        Impl* owner_;
        Connection* connection_;
        bool writer_;
        const Lease* outer_;
    };

    explicit Database(std::unique_ptr<Impl> impl) noexcept;

    [[nodiscard]] Result<void> ensure_no_transaction_on_this_thread() const;
    // Applies one migration (see migrate()) when the database is older.
    [[nodiscard]] Result<void> apply(const Migration& migration);
    [[nodiscard]] Lease lease_reader();
    [[nodiscard]] Lease lease_writer();
    [[nodiscard]] static Result<void> run(Connection& connection, std::string_view sql);

    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Template implementations
// ---------------------------------------------------------------------------
template <class F>
auto Database::read(F&& fn) -> std::invoke_result_t<F, ReadTx&> {
    static_assert(is_result_v<std::remove_cvref_t<std::invoke_result_t<F, ReadTx&>>>,
                  "the callback passed to Database::read() must return rr::Result<T>");
    RR_TRY(ensure_no_transaction_on_this_thread());
    const Lease lease = lease_reader();
    // A deferred transaction pins one consistent WAL snapshot for every
    // statement inside it. The lease ends it on release.
    RR_TRY(run(lease.connection(), "BEGIN"));
    ReadTx tx{lease.connection()};
    return std::invoke(std::forward<F>(fn), tx);
}

template <class F>
auto Database::write(F&& fn) -> std::invoke_result_t<F, WriteTx&> {
    static_assert(is_result_v<std::remove_cvref_t<std::invoke_result_t<F, WriteTx&>>>,
                  "the callback passed to Database::write() must return rr::Result<T>");
    RR_TRY(ensure_no_transaction_on_this_thread());
    const Lease lease = lease_writer();
    RR_TRY(run(lease.connection(), "BEGIN IMMEDIATE"));
    WriteTx tx{lease.connection()};
    auto result = std::invoke(std::forward<F>(fn), tx);
    if (result.has_value()) {
        RR_TRY(run(lease.connection(), "COMMIT"));
    }
    // On failure, on a failed COMMIT, or when `fn` throws, the lease rolls
    // the transaction back: nothing half-written survives.
    return result;
}

}  // namespace rr::db
