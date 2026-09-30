// rr/library/library.hpp — a library's database, created and brought up to date.
//
// Why this exists
//   Each library is one SQLite file (ADR-0008). Its schema is the numbered
//   SQL scripts in migrations/ (NNNN_name.sql), compiled into the binary and
//   applied in order, each in one transaction (rr_db). open_database() is the
//   one way in: it creates the file when needed, applies what is pending,
//   and refuses a database written by a newer rReport rather than misread it.
//
//   Scripts are append-only once released: a released database has run
//   them, so a change goes into a new script.
//
// Thread-safety: plain functions.
#pragma once

#include <filesystem>
#include <span>

#include <rr/core/result.hpp>
#include <rr/db/database.hpp>

namespace rr::library {

// The schema's migrations, in order (versions 1, 2, …).
[[nodiscard]] std::span<const db::Migration> migrations();

// Opens the library database at `file` (created if missing, with its
// directory) and applies pending migrations.
[[nodiscard]] Result<db::Database> open_database(const std::filesystem::path& file);

}  // namespace rr::library
