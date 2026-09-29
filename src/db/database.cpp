#include <rr/db/database.hpp>

#include <algorithm>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <format>
#include <limits>
#include <mutex>
#include <unordered_map>

#include <sqlite3.h>

#include <rr/core/strings.hpp>

namespace rr::db {

namespace {

struct HandleCloser {
    void operator()(sqlite3* handle) const noexcept { sqlite3_close_v2(handle); }
};
struct StatementFinalizer {
    void operator()(sqlite3_stmt* statement) const noexcept { sqlite3_finalize(statement); }
};
struct SqliteFree {
    void operator()(char* text) const noexcept { sqlite3_free(text); }
};
using HandlePtr = std::unique_ptr<sqlite3, HandleCloser>;
using StatementPtr = std::unique_ptr<sqlite3_stmt, StatementFinalizer>;

// Heterogeneous lookup: find a cached statement by string_view without
// allocating a std::string for every query.
struct TransparentHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view text) const noexcept {
        return std::hash<std::string_view>{}(text);
    }
};

struct CachedStatement {
    StatementPtr statement;
    bool in_use = false;  // checked out by a query that is still running
};

// The SQL text is the cache key. The cap only matters for dynamically built
// SQL (e.g. IN lists of varying length); beyond it, statements run uncached.
constexpr std::size_t kMaxCachedStatements = 512;

constexpr std::size_t kNoRowLimit = std::numeric_limits<std::size_t>::max();

enum class Access : std::uint8_t {
    read_only,
    read_write
};

}  // namespace

// One sqlite3 handle and its prepared-statement cache. Used by one thread at
// a time: the Database hands connections out through leases.
struct Connection {
    // Declared first so it is destroyed last, after every cached statement has
    // been finalized. (sqlite3_close_v2 would tolerate the other order, but
    // only by deferring the close; this way the handle really closes here.)
    HandlePtr handle;
    std::unordered_map<std::string, CachedStatement, TransparentHash, std::equal_to<>> statements;

    [[nodiscard]] sqlite3* get() const noexcept { return handle.get(); }
};

namespace {

// Maps SQLite result codes to error classes (ADR-0007).
Errc classify(int code) noexcept {
    switch (code) {  // extended codes that refine their primary class
        case SQLITE_CONSTRAINT_NOTNULL:
        case SQLITE_CONSTRAINT_CHECK:
        case SQLITE_CONSTRAINT_DATATYPE:
            return Errc::invalid_argument;  // the value itself is wrong, whatever the table holds
        default:
            break;
    }
    switch (code & 0xFF) {  // the primary code of an extended result code
        case SQLITE_BUSY:
        case SQLITE_LOCKED:
            return Errc::unavailable;  // transient: another connection holds a lock
        case SQLITE_CONSTRAINT:
            return Errc::conflict;  // UNIQUE, PRIMARY KEY, FOREIGN KEY: clashes with existing data
        case SQLITE_CORRUPT:
        case SQLITE_NOTADB:
            return Errc::integrity;
        case SQLITE_CANTOPEN:
        case SQLITE_IOERR:
        case SQLITE_FULL:
        case SQLITE_READONLY:
        case SQLITE_PERM:
            return Errc::io;
        case SQLITE_MISMATCH:
        case SQLITE_RANGE:
        case SQLITE_TOOBIG:
            return Errc::invalid_argument;
        case SQLITE_INTERRUPT:
            return Errc::cancelled;
        default:
            return Errc::internal;
    }
}

std::unexpected<Error> sqlite_error(const Connection& connection, int code, std::string_view what,
                                    std::source_location where = std::source_location::current()) {
    const char* detail =
        connection.get() != nullptr ? sqlite3_errmsg(connection.get()) : sqlite3_errstr(code);
    return fail(classify(code), std::format("{}: {} (sqlite code {})", what, detail, code), where);
}

// True when `rest` holds no statement: only whitespace and comments.
bool holds_no_statement(const Connection& connection, std::string_view rest) {
    if (trim(rest).empty()) {
        return true;
    }
    sqlite3_stmt* raw = nullptr;
    const int code =
        sqlite3_prepare_v3(connection.get(), rest.data(), static_cast<int>(rest.size()), 0, &raw, nullptr);
    const StatementPtr extra{raw};
    return code == SQLITE_OK && extra == nullptr;
}

// Prepares exactly one statement. Trailing whitespace and comments are fine;
// a second statement is an error, because silently ignoring it (what the C
// API does) hides bugs.
Result<StatementPtr> prepare(const Connection& connection, std::string_view sql, bool persistent) {
    if (!std::in_range<int>(sql.size())) {
        return fail(Errc::invalid_argument, "SQL statement too long");
    }
    sqlite3_stmt* raw = nullptr;
    const char* tail = nullptr;
    const unsigned int flags = persistent ? SQLITE_PREPARE_PERSISTENT : 0U;
    const int code =
        sqlite3_prepare_v3(connection.get(), sql.data(), static_cast<int>(sql.size()), flags, &raw, &tail);
    StatementPtr statement{raw};
    if (code != SQLITE_OK) {
        return sqlite_error(connection, code, std::format("cannot prepare \"{}\"", sql));
    }
    if (statement == nullptr) {
        return fail(Errc::invalid_argument, "empty SQL statement");
    }
    const std::string_view rest{tail, sql.data() + sql.size()};
    if (!holds_no_statement(connection, rest)) {
        return fail(Errc::invalid_argument,
                    std::format("\"{}\" contains more than one statement; use execute_script()", sql));
    }
    return statement;
}

// A prepared statement checked out for one query.
//   * A cached statement goes back to the cache: reset (which also releases
//     any read lock it holds), bindings cleared, marked idle.
//   * A one-off statement is finalized.
class CheckedOutStatement {
public:
    explicit CheckedOutStatement(CachedStatement& entry) noexcept
        : statement_(entry.statement.get())
        , entry_(&entry) {
        entry.in_use = true;
    }
    explicit CheckedOutStatement(StatementPtr one_off) noexcept
        : statement_(one_off.get())
        , owned_(std::move(one_off)) {}

    CheckedOutStatement(CheckedOutStatement&& other) noexcept
        : statement_(std::exchange(other.statement_, nullptr))
        , entry_(std::exchange(other.entry_, nullptr))
        , owned_(std::move(other.owned_)) {}
    CheckedOutStatement& operator=(CheckedOutStatement&&) = delete;
    CheckedOutStatement(const CheckedOutStatement&) = delete;
    CheckedOutStatement& operator=(const CheckedOutStatement&) = delete;

    ~CheckedOutStatement() {
        if (statement_ == nullptr) {
            return;  // moved from
        }
        sqlite3_reset(statement_);
        sqlite3_clear_bindings(statement_);
        if (entry_ != nullptr) {
            entry_->in_use = false;
        }
        // owned_, if set, finalizes a one-off statement.
    }

    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

private:
    sqlite3_stmt* statement_;
    CachedStatement* entry_ = nullptr;  // unordered_map nodes never move, so this stays valid
    StatementPtr owned_;
};

Result<CheckedOutStatement> checkout(Connection& connection, std::string_view sql) {
    const auto found = connection.statements.find(sql);
    if (found != connection.statements.end() && !found->second.in_use) {
        return CheckedOutStatement{found->second};
    }
    // Either not cached yet, or busy in an enclosing query running the same
    // SQL (a recursive walk over a tree). Sharing the busy statement would
    // reset the enclosing query halfway through, so that case runs one-off.
    const bool cache =
        found == connection.statements.end() && connection.statements.size() < kMaxCachedStatements;
    RR_TRY_ASSIGN(StatementPtr statement, prepare(connection, sql, cache));
    if (!cache) {
        return CheckedOutStatement{std::move(statement)};
    }
    const auto inserted =
        connection.statements.emplace(std::string{sql}, CachedStatement{.statement = std::move(statement)})
            .first;
    return CheckedOutStatement{inserted->second};
}

// A null destructor (SQLITE_STATIC): SQLite reads the caller's buffer instead
// of copying it — worthwhile for multi-megabyte artifact blobs. Safe because
// every Value outlives the statement's use: bindings are cleared before
// run_statement() returns.
constexpr sqlite3_destructor_type kBindWithoutCopy = nullptr;

int bind_one(sqlite3_stmt* statement, int position, const Value& value) {
    return std::visit(
        [&]<class T>(const T& held) -> int {
            if constexpr (std::is_same_v<T, std::nullptr_t>) {
                return sqlite3_bind_null(statement, position);
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return sqlite3_bind_int64(statement, position, held);
            } else if constexpr (std::is_same_v<T, double>) {
                return sqlite3_bind_double(statement, position, held);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return sqlite3_bind_text64(statement, position, held.data(), held.size(), kBindWithoutCopy,
                                           SQLITE_UTF8);
            } else {
                static_assert(std::is_same_v<T, Blob>);
                // An empty vector's data() may be nullptr, which SQLite binds
                // as NULL; an empty blob must stay an empty blob.
                if (held.empty()) {
                    return sqlite3_bind_zeroblob(statement, position, 0);
                }
                return sqlite3_bind_blob64(statement, position, held.data(), held.size(), kBindWithoutCopy);
            }
        },
        value.storage());
}

Result<void> bind_all(const Connection& connection, sqlite3_stmt* statement, std::span<const Value> params) {
    const int expected = sqlite3_bind_parameter_count(statement);
    if (!std::cmp_equal(expected, params.size())) {
        return fail(Errc::invalid_argument,
                    std::format("statement expects {} parameter(s), {} given", expected, params.size()));
    }
    int position = 0;
    for (const Value& value : params) {
        ++position;
        const int code = bind_one(statement, position, value);
        if (code != SQLITE_OK) {
            return sqlite_error(connection, code, std::format("cannot bind parameter ?{}", position));
        }
    }
    return ok();
}

// Runs one statement, calling `on_row` (if set) for at most `row_limit` rows.
Result<void> run_statement(Connection& connection, std::string_view sql, std::span<const Value> params,
                           Access access, const RowCallback& on_row, std::size_t row_limit) {
    RR_TRY_ASSIGN(const CheckedOutStatement statement, checkout(connection, sql));
    if (access == Access::read_only && sqlite3_stmt_readonly(statement.get()) == 0) {
        return fail(Errc::invalid_argument,
                    std::format("\"{}\" would modify the database; use Database::write()", sql));
    }
    RR_TRY(bind_all(connection, statement.get(), params));
    for (std::size_t rows = 0; rows < row_limit; ++rows) {
        const int code = sqlite3_step(statement.get());
        if (code == SQLITE_DONE) {
            return ok();
        }
        if (code != SQLITE_ROW) {
            return sqlite_error(connection, code, std::format("\"{}\" failed", sql));
        }
        if (on_row) {
            RR_TRY(on_row(Row{statement.get()}));
        }
    }
    return ok();  // row limit reached; the statement is reset on checkin
}

Result<std::optional<std::string>> first_text(Connection& connection, std::string_view sql,
                                              std::span<const Value> params, Access access) {
    std::optional<std::string> value;
    RR_TRY(run_statement(
        connection, sql, params, access,
        [&](const Row& row) -> Result<void> {
            value = row.get_optional_text(0);
            return ok();
        },
        1));
    return value;
}

Result<std::optional<std::int64_t>> first_int(Connection& connection, std::string_view sql,
                                              std::span<const Value> params, Access access) {
    std::optional<std::int64_t> value;
    RR_TRY(run_statement(
        connection, sql, params, access,
        [&](const Row& row) -> Result<void> {
            value = row.get_optional_int(0);
            return ok();
        },
        1));
    return value;
}

Result<std::unique_ptr<Connection>> open_connection(const std::filesystem::path& path, Access access,
                                                    std::chrono::milliseconds busy_timeout) {
    auto connection = std::make_unique<Connection>();
    // NOMUTEX: a connection is used by one thread at a time (leases guarantee
    // it), so SQLite's own per-call locking would be pure overhead.
    // EXRESCODE: extended result codes (e.g. SQLITE_CONSTRAINT_UNIQUE) everywhere.
    const int mode =
        access == Access::read_only ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    const int flags = mode | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE;
    sqlite3* raw = nullptr;
    const int code = sqlite3_open_v2(path.string().c_str(), &raw, flags, nullptr);
    connection->handle.reset(raw);  // even a failed open returns a handle that must be closed
    if (code != SQLITE_OK) {
        return sqlite_error(*connection, code, std::format("cannot open database '{}'", path.string()));
    }
    const auto timeout_ms = std::clamp<std::int64_t>(busy_timeout.count(), 0, INT_MAX);
    sqlite3_busy_timeout(raw, static_cast<int>(timeout_ms));
    return connection;
}

// Runs ';'-separated statements without parameters.
Result<void> run_script(const Connection& connection, std::string_view sql) {
    const std::string text{sql};  // sqlite3_exec needs a NUL-terminated string
    char* raw_message = nullptr;
    const int code = sqlite3_exec(connection.get(), text.c_str(), nullptr, nullptr, &raw_message);
    const std::unique_ptr<char, SqliteFree> message{raw_message};
    if (code != SQLITE_OK) {
        return fail(classify(code),
                    std::format("script failed: {} (sqlite code {})",
                                message != nullptr ? message.get() : sqlite3_errstr(code), code));
    }
    return ok();
}

Access access_for(bool writable) noexcept {
    return writable ? Access::read_write : Access::read_only;
}

sqlite3_stmt* as_statement(void* opaque) noexcept {
    return static_cast<sqlite3_stmt*>(opaque);
}

}  // namespace

// ---------------------------------------------------------------------------
// Row
// ---------------------------------------------------------------------------
int Row::column_count() const noexcept {
    return sqlite3_column_count(as_statement(statement_));
}

bool Row::is_null(int column) const noexcept {
    return sqlite3_column_type(as_statement(statement_), column) == SQLITE_NULL;
}

std::int64_t Row::get_int(int column) const noexcept {
    return sqlite3_column_int64(as_statement(statement_), column);
}

double Row::get_double(int column) const noexcept {
    return sqlite3_column_double(as_statement(statement_), column);
}

std::string Row::get_text(int column) const {
    // Order matters: sqlite3_column_bytes() must follow the conversion done
    // by sqlite3_column_text(), or it may report the size of the old format.
    const unsigned char* text = sqlite3_column_text(as_statement(statement_), column);
    const int size = sqlite3_column_bytes(as_statement(statement_), column);
    if (text == nullptr) {
        return {};
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — SQLite returns UTF-8 as unsigned char.
    return std::string{reinterpret_cast<const char*>(text), static_cast<std::size_t>(size)};
}

Blob Row::get_blob(int column) const {
    const void* data = sqlite3_column_blob(as_statement(statement_), column);
    const int size = sqlite3_column_bytes(as_statement(statement_), column);
    Blob blob(static_cast<std::size_t>(size));
    if (data != nullptr && size > 0) {
        std::memcpy(blob.data(), data, blob.size());
    }
    return blob;
}

std::optional<std::int64_t> Row::get_optional_int(int column) const noexcept {
    if (is_null(column)) {
        return std::nullopt;
    }
    return get_int(column);
}

std::optional<double> Row::get_optional_double(int column) const noexcept {
    if (is_null(column)) {
        return std::nullopt;
    }
    return get_double(column);
}

std::optional<std::string> Row::get_optional_text(int column) const {
    if (is_null(column)) {
        return std::nullopt;
    }
    return get_text(column);
}

// ---------------------------------------------------------------------------
// ReadTx / WriteTx
// ---------------------------------------------------------------------------
Result<void> ReadTx::query(std::string_view sql, std::span<const Value> params, const RowCallback& on_row) {
    return run_statement(connection(), sql, params, access_for(writable_), on_row, kNoRowLimit);
}

Result<std::optional<std::int64_t>> ReadTx::query_optional_int(std::string_view sql,
                                                               std::initializer_list<Value> params) {
    return first_int(connection(), sql, {params.begin(), params.size()}, access_for(writable_));
}

Result<std::optional<std::string>> ReadTx::query_optional_text(std::string_view sql,
                                                               std::initializer_list<Value> params) {
    return first_text(connection(), sql, {params.begin(), params.size()}, access_for(writable_));
}

Result<std::int64_t> ReadTx::query_int(std::string_view sql, std::initializer_list<Value> params) {
    RR_TRY_ASSIGN(const std::optional<std::int64_t> value, query_optional_int(sql, params));
    if (!value.has_value()) {
        return fail(Errc::not_found, std::format("no value for \"{}\"", sql));
    }
    return *value;
}

Result<std::string> ReadTx::query_text(std::string_view sql, std::initializer_list<Value> params) {
    RR_TRY_ASSIGN(std::optional<std::string> value, query_optional_text(sql, params));
    if (!value.has_value()) {
        return fail(Errc::not_found, std::format("no value for \"{}\"", sql));
    }
    return std::move(*value);
}

Result<std::int64_t> WriteTx::execute(std::string_view sql, std::span<const Value> params) {
    sqlite3* handle = connection().get();
    const sqlite3_int64 total_before = sqlite3_total_changes64(handle);
    RR_TRY(run_statement(connection(), sql, params, Access::read_write, {}, kNoRowLimit));
    // sqlite3_changes64() describes the most recent INSERT/UPDATE/DELETE, which
    // may be an *earlier* statement. If the connection's running total did not
    // move, this statement changed nothing, whatever kind it was.
    if (sqlite3_total_changes64(handle) == total_before) {
        return 0;
    }
    return sqlite3_changes64(handle);
}

Result<void> WriteTx::execute_script(std::string_view sql) {
    RR_TRY(run_script(connection(), sql));
    if (sqlite3_get_autocommit(connection().get()) != 0) {
        return fail(Errc::internal,
                    "the script ended the enclosing transaction; scripts must not contain BEGIN, COMMIT or "
                    "ROLLBACK");
    }
    return ok();
}

std::int64_t WriteTx::last_insert_rowid() const noexcept {
    return sqlite3_last_insert_rowid(connection().get());
}

// ---------------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------------
struct Database::Impl {
    std::filesystem::path path;

    std::mutex writer_mutex;                // serializes all use of `writer`
    std::mutex pool_mutex;                  // guards `free_readers`
    std::condition_variable pool_cv;        // signalled whenever a reader is returned
    std::vector<Connection*> free_readers;  // capacity reserved up front: returning one never allocates

    // Members are destroyed in reverse order: readers close before the writer,
    // so the last connection standing is the one able to checkpoint the WAL
    // and remove the -wal/-shm files.
    std::unique_ptr<Connection> writer;
    std::vector<std::unique_ptr<Connection>> readers;
};

const Database::Lease*& Database::Lease::innermost() noexcept {
    static thread_local const Lease* top = nullptr;
    return top;
}

Database::Lease::Lease(Impl& owner, Connection& connection, bool writer) noexcept
    : owner_(&owner)
    , connection_(&connection)
    , writer_(writer)
    , outer_(innermost()) {
    innermost() = this;
}

Database::Lease::~Lease() {
    // Leases are locals of read()/write(), so they are released in LIFO order.
    RR_ASSERT(innermost() == this, "database leases must be released in LIFO order");
    innermost() = outer_;

    // Invariant: a connection is returned in autocommit mode. This ends read
    // transactions and rolls back failed or abandoned writes. Should ROLLBACK
    // itself fail there is nothing better to do here; the next BEGIN on this
    // connection would report the problem.
    if (sqlite3_get_autocommit(connection_->get()) == 0) {
        (void)sqlite3_exec(connection_->get(), "ROLLBACK", nullptr, nullptr, nullptr);
    }
    if (writer_) {
        owner_->writer_mutex.unlock();
        return;
    }
    {
        const std::lock_guard lock(owner_->pool_mutex);
        owner_->free_readers.push_back(connection_);
    }
    owner_->pool_cv.notify_one();
}

bool Database::Lease::held_by_this_thread(const Impl& owner) noexcept {
    for (const Lease* lease = innermost(); lease != nullptr; lease = lease->outer_) {
        if (lease->owner_ == &owner) {
            return true;
        }
    }
    return false;
}

Database::Database(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Database::Database(Database&&) noexcept = default;
Database& Database::operator=(Database&&) noexcept = default;
Database::~Database() = default;

Result<Database> Database::open(const Options& options) {
    auto impl = std::make_unique<Impl>();
    impl->path = options.path;
    const bool in_memory = options.path == ":memory:";

    RR_TRY_ASSIGN(impl->writer, open_connection(options.path, Access::read_write, options.busy_timeout));
    // WAL lets readers work on a snapshot while the writer commits. SQLite
    // answers with the mode actually in effect, which is not WAL on
    // filesystems without shared-memory support; better to refuse than to
    // run with readers and writer blocking each other.
    RR_TRY_ASSIGN(const std::optional<std::string> journal_mode,
                  first_text(*impl->writer, "PRAGMA journal_mode = WAL", {}, Access::read_write));
    if (!in_memory && journal_mode != "wal") {
        return fail(Errc::unsupported,
                    std::format("cannot use WAL mode for '{}' (journal mode is '{}'); is it on a network "
                                "filesystem?",
                                options.path.string(), journal_mode.value_or("unknown")));
    }
    // NORMAL synchronous is durable at WAL checkpoints: a power cut can lose
    // the last transactions but never corrupts the file — the right trade-off
    // for a library whose contents can be recomputed.
    RR_TRY(run_script(*impl->writer,
                      "PRAGMA synchronous = NORMAL; PRAGMA foreign_keys = ON; PRAGMA temp_store = MEMORY;"));

    // A private ":memory:" database cannot be shared between connections, so
    // reads then go through the writer connection.
    if (!in_memory) {
        impl->readers.reserve(options.reader_count);
        impl->free_readers.reserve(options.reader_count);
        for (std::size_t index = 0; index < options.reader_count; ++index) {
            RR_TRY_ASSIGN(auto reader,
                          open_connection(options.path, Access::read_only, options.busy_timeout));
            RR_TRY(run_script(*reader, "PRAGMA temp_store = MEMORY;"));
            impl->free_readers.push_back(reader.get());
            impl->readers.push_back(std::move(reader));
        }
    }
    return Database{std::move(impl)};
}

Result<void> Database::ensure_no_transaction_on_this_thread() const {
    if (Lease::held_by_this_thread(*impl_)) {
        return fail(Errc::internal,
                    std::format("nested transaction on '{}': inside read()/write(), use the ReadTx/WriteTx "
                                "you were given",
                                impl_->path.string()));
    }
    return ok();
}

Database::Lease Database::lease_reader() {
    if (impl_->readers.empty()) {
        return lease_writer();  // in-memory database, or reader_count == 0
    }
    std::unique_lock lock(impl_->pool_mutex);
    impl_->pool_cv.wait(lock, [this] { return !impl_->free_readers.empty(); });
    Connection* connection = impl_->free_readers.back();
    impl_->free_readers.pop_back();
    return Lease{*impl_, *connection, false};
}

Database::Lease Database::lease_writer() {
    impl_->writer_mutex.lock();  // unlocked by ~Lease
    return Lease{*impl_, *impl_->writer, true};
}

Result<void> Database::run(Connection& connection, std::string_view sql) {
    return run_statement(connection, sql, {}, Access::read_write, {}, kNoRowLimit);
}

Result<int> Database::schema_version() {
    return read([](ReadTx& tx) -> Result<int> {
        RR_TRY_ASSIGN(const std::int64_t version, tx.query_int("PRAGMA user_version"));
        return static_cast<int>(version);
    });
}

Result<bool> Database::supports_fts5() {
    return read([](ReadTx& tx) -> Result<bool> {
        RR_TRY_ASSIGN(const std::int64_t used,
                      tx.query_int("SELECT sqlite_compileoption_used('ENABLE_FTS5')"));
        return used == 1;
    });
}

Result<int> Database::migrate(std::span<const Migration> migrations) {
    for (std::size_t index = 0; index < migrations.size(); ++index) {
        if (!std::cmp_equal(migrations[index].version, index + 1)) {
            return fail(Errc::internal,
                        std::format("migration '{}' has version {}, expected {}", migrations[index].name,
                                    migrations[index].version, index + 1));
        }
    }
    const int latest = static_cast<int>(migrations.size());
    RR_TRY_ASSIGN(const int current, schema_version());
    if (current < 0) {
        return fail(Errc::integrity, std::format("database '{}' has an invalid schema version {}",
                                                 impl_->path.string(), current));
    }
    if (current > latest) {
        return fail(Errc::conflict,
                    std::format("database '{}' has schema v{}, newer than this program (v{}); "
                                "upgrade rReport instead of downgrading the data",
                                impl_->path.string(), current, latest));
    }
    for (const Migration& migration : migrations.subspan(static_cast<std::size_t>(current))) {
        auto applied = write([&](WriteTx& tx) -> Result<void> {
            // Checked again under the write lock: another process may have
            // applied this migration since we read the version.
            RR_TRY_ASSIGN(const std::int64_t version, tx.query_int("PRAGMA user_version"));
            if (version >= migration.version) {
                return ok();
            }
            RR_TRY(tx.execute_script(migration.sql));
            // user_version lives in the database header, which is transactional:
            // it commits (or rolls back) together with the migration.
            return tx.execute_script(std::format("PRAGMA user_version = {}", migration.version));
        });
        if (!applied.has_value()) {
            return fail(std::move(applied).error().with_context(
                std::format("while applying migration {} ({})", migration.version, migration.name)));
        }
    }
    return latest;
}

const std::filesystem::path& Database::path() const noexcept {
    return impl_->path;
}

}  // namespace rr::db
