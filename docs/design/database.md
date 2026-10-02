# rr_db: how the SQLite layer works, and why

`rr_db` (`src/db/`) is the only code that talks to the SQLite C API. ADR-0008 records the decision to use one SQLite file per library. This note explains the mechanisms inside the wrapper, so that nobody "simplifies" one away without knowing what it protects.

## The API in one screen

```cpp
Database db = unwrap(Database::open({.path = library_dir / "library.db"}));
RR_TRY(db.migrate(kMigrations));                       // numbered scripts, one transaction each

RR_TRY(db.write([&](WriteTx& tx) -> Result<void> {     // BEGIN IMMEDIATE … COMMIT
    RR_TRY(tx.execute("INSERT INTO jobs(id, kind) VALUES (?1, ?2)", {job_id, "ingest"}));
    return ok();                                       // an error or exception → ROLLBACK
}));

RR_TRY_ASSIGN(const std::int64_t jobs, db.read([](ReadTx& tx) {   // one consistent snapshot
    return tx.query_int("SELECT COUNT(*) FROM jobs");
}));
```

- **Transactions are callbacks.** There is no `begin()`/`commit()` pair to forget. The callback's `Result` decides: success commits; failure or an exception rolls back.
- **Parameters are `Value`s** (`rr/db/value.hpp`). Implicit conversions map C++ types to SQLite's five storage classes, deliberately excluding:
  - characters: `'x'` is not the number 120;
  - pointers: they would silently become booleans;
  - durations: a number without its unit.

## Connections and locking

```
                ┌──────────────────────── Database ─────────────────────────┐
  write(fn) ──▶ │ writer_mutex ──▶ writer connection (read-write)           │ BEGIN IMMEDIATE … COMMIT
                │                                                           │
  read(fn)  ──▶ │ pool_mutex/cv ─▶ N reader connections (SQLITE_OPEN_READONLY)│ BEGIN … ROLLBACK
                └───────────────────────────────────────────────────────────┘
```

| Mechanism | Why |
|---|---|
| **WAL journal mode** | Readers see a snapshot while the writer appends to the log, so neither blocks the other. `open()` checks the mode SQLite actually granted. Filesystems without shared memory (network mounts) silently fall back to a rollback journal, where readers and writers block each other; we refuse with `Errc::unsupported` instead. |
| **One writer, serialized by a mutex** | SQLite allows one writer at a time anyway. Queueing writers in our process is cheaper and fairer than letting them collide inside SQLite and retry `SQLITE_BUSY`. |
| **`BEGIN IMMEDIATE`** | A deferred transaction takes the write lock at its first write. If another *process* holds it, the transaction fails halfway through. `IMMEDIATE` takes the lock up front, waiting up to `busy_timeout`, so a write either starts cleanly or not at all. |
| **Read-only reader connections** | A reader physically cannot write. `ReadTx` also rejects any statement for which `sqlite3_stmt_readonly()` is false. That matters for `:memory:` databases, whose reads share the writer connection. |
| **`synchronous = NORMAL`** | In WAL mode this is durable at checkpoints: a power cut can lose the last few commits but never corrupts the file. Right for a library whose contents can be recomputed from artifacts. |
| **`foreign_keys = ON`** | SQLite leaves foreign keys *off* by default, for backward compatibility. The test suite checks that a dangling reference is rejected. |

## Leases: the one place connections are handed out

`read()` and `write()` obtain a connection through a `Lease`, an immovable RAII object that lives in their stack frame. Its destructor enforces one invariant:

> **A connection returns to its pool in autocommit mode.**

If the connection is still inside a transaction when the lease ends, the destructor issues `ROLLBACK`. This single rule covers every path:

- it is how read transactions end;
- it rolls back failed writes;
- it rolls back writes whose `COMMIT` failed;
- it rolls back writes whose callback threw.

The write path only has to say `COMMIT` on success. Nothing else in the code base can leave a connection mid-transaction.

### Nested transactions are errors, detected without allocation

Calling `db.read()` inside a `db.write()` callback on the same database would go wrong in one of three ways:

- **Deadlock:** in-memory reads need the writer mutex, which this thread already holds, and `std::mutex` is not recursive.
- **Deadlock:** exhaust a small reader pool from a single thread.
- **Silent staleness:** worse, read a snapshot that does not contain the enclosing, uncommitted write.

So it is refused with `Errc::internal` ("use the ReadTx/WriteTx you were given").

Detection uses an **intrusive per-thread stack**:

- a `thread_local` pointer holds the innermost lease;
- each lease stores a pointer to the one it displaced (`outer_`);
- checking for nesting walks this short chain.

No allocation and no locks, and the chain lives entirely in the stack frames of the active transactions. Transactions on *different* databases may nest (copying between libraries).

## The statement cache, and nested queries over the same SQL

Preparing SQL costs more than running it, so each connection caches prepared statements, keyed by SQL text. Lookups use heterogeneous `string_view` lookup, so they allocate nothing.

The subtle case is a row callback that runs **the same SQL again**, for example a recursive walk down the idea tree:

```cpp
walk = [&](std::int64_t parent) {
    return tx.query("SELECT id FROM ideas WHERE parent_id = ?1", {parent}, [&](const Row& row) {
        return walk(row.get_int(0));   // same SQL, while the outer query is mid-iteration
    });
};
```

- **The failure mode.** If the inner call reused the cached statement, its reset would rewind the *outer* query. The walk would then loop forever or skip rows.
- **The fix.** Each cache entry carries an `in_use` flag. A busy statement is never shared; the nested call gets a one-off statement, finalized afterwards.
- **The cap.** The cache is capped at 512 entries. Beyond that, dynamically built SQL runs uncached rather than growing memory without bound.

## Binding without copies

Parameters are bound with a null destructor (`SQLITE_STATIC`), so SQLite reads the caller's buffer instead of copying it. This matters for multi-megabyte artifact blobs.

It is safe because every `Value` outlives the statement's use: bindings are cleared before the query function returns.

One trap is handled explicitly: an empty `std::vector` may have `data() == nullptr`, which SQLite would bind as NULL. Empty blobs therefore go through `sqlite3_bind_zeroblob`.

## Error classification

| SQLite result | `Errc` | Meaning for the caller |
|---|---|---|
| `BUSY`, `LOCKED` | `unavailable` | Transient: another process holds a lock. Retry with backoff. |
| `CONSTRAINT_UNIQUE`, `_PRIMARYKEY`, `_FOREIGNKEY` | `conflict` | Clashes with existing data ("already exists" can often be resolved). |
| `CONSTRAINT_NOTNULL`, `_CHECK`, `_DATATYPE` | `invalid_argument` | The value itself is wrong, whatever the table holds. |
| `CORRUPT`, `NOTADB` | `integrity` | The file is damaged or not a database. |
| `CANTOPEN`, `IOERR`, `FULL`, `READONLY`, `PERM` | `io` | Filesystem trouble. |
| `INTERRUPT` | `cancelled` | |
| Syntax errors and everything else | `internal` | A bug in our SQL. |

`WriteTx::execute()` returns the rows changed by *this* statement. SQLite's `sqlite3_changes64()` reports the most recent INSERT/UPDATE/DELETE, which may be an earlier statement. So `execute()` also compares the connection's running total and returns 0 when nothing moved.

## Migrations

- **Order.** Versions must be exactly 1..N. This is validated before anything runs.
- **One transaction each.** Each migration runs in its own write transaction, together with `PRAGMA user_version = N`. The version lives in the database header, which is transactional, so a failed migration leaves neither half its tables nor a bumped version.
- **Concurrent processes.** The version is re-checked under the write lock, so two processes starting at once cannot apply the same migration twice.
- **Newer databases.** A database newer than the program is refused (`Errc::conflict`). Downgrading data is never silent.
- **No transaction control.** A script must not contain `BEGIN`/`COMMIT`/`ROLLBACK`. If one ends the enclosing transaction, that is detected right after the script runs and reported as a bug.
- **Foreign keys off, and checked before the commit** (P24). SQLite's `ALTER TABLE` cannot change a CHECK, among much else: the table is made anew, its rows copied, the old one dropped and the new one renamed, [SQLite's own procedure](https://www.sqlite.org/lang_altertable.html#otheralter). With foreign keys on, that fails for a table others refer to, and deferring them does not help, as tried with `sqlite3`: dropping the old table breaks every reference to it, SQLite counts them, and renaming the new table does not count them back, so the `COMMIT` fails though the data is whole. So `apply()` turns foreign keys off on the writer before the transaction (`PRAGMA foreign_keys` does nothing inside one), runs the migration, and counts `pragma_foreign_key_check` before committing: a migration that leaves a reference broken fails with `Errc::integrity`, naming each pair of tables (`tags → items (1)`), and is rolled back.
- **On again, after the transaction ends.** Foreign keys are turned back on after the commit, or after the rollback of a migration that failed, never before it: inside the still-open transaction the pragma would do nothing, and the writer would go on without them, silently. A `ROLLBACK` that itself failed is returned as the error, and the database is not used further.

## Closing order

`Impl` declares the writer before the readers, so the readers are destroyed first and the writer closes last. This matters:

- only the last connection to close can checkpoint the WAL and delete the `-wal`/`-shm` files;
- a read-only connection cannot checkpoint, so it would leave the log behind, un-checkpointed.

Verified empirically. The test `FileDatabase.UsesWalPersistsDataAndClosesCleanly` pins the behaviour.

## How the tests were validated

Every protective mechanism above was **mutation-tested**: the protection was removed, and the matching test was confirmed to fail.

| Mechanism removed | Test that caught it |
|---|---|
| `in_use` check on cached statements | `NestedQueriesMayRunTheSameSql` |
| Writer-closes-last member order | `UsesWalPersistsDataAndClosesCleanly` |
| Running-total check in `execute()` | `ExecuteCountsOnlyRowsThisStatementChanged` |
| `zeroblob` for empty blobs | `ValuesRoundTripThroughEveryStorageClass` |
| Read-only check in `ReadTx` | `ReadTransactionsCannotWrite` |
| Second-statement detection | `StatementMistakesAreReportedNotIgnored` |
| Script-ended-transaction detection | `AScriptMustNotEndTheTransaction` |
| `ROLLBACK` on lease release | `AnExceptionRollsBackAndReleasesTheWriter` |
| Constraint sub-classification | `ConstraintViolationsAreClassified` |
| Foreign keys off while migrating (P24) | `MayRebuildATableOthersReferToKeepingItsRowsAndTheirReferences` |
| The references checked before a migration commits | `AMigrationThatBreaksAReferenceIsRefusedAndRolledBack` |
| Foreign keys on again after a migration | `MayRebuildATableOthersReferToKeepingItsRowsAndTheirReferences` |
| On again only after the failed transaction ends | `ForeignKeysAreOnAgainAfterAMigrationThatFails` |
| The failed transaction rolled back before they are | `AMigrationThatBreaksAReferenceIsRefusedAndRolledBack` |

The concurrency tests (snapshot isolation, serialized writers, reader-pool waits) also run under ThreadSanitizer and were repeated 100 times without a failure.

The mutants of P24's rows are kept in `tests/mutants/db.toml`, so `scripts/mutate.py` runs them again; the earlier rows were run by hand, before those specs existed.
