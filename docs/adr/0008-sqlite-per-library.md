# ADR-0008: One SQLite database per library

- Status: Accepted
- Date: 2026-09-29

## Context

Each user's library (videos, analyses, research, conversations) is independent. There are no cross-user queries. The hub runs on a laptop today and becomes a multi-tenant hosted service later.

## Decision

- **One file per library.** Each library is one SQLite file (`library.db`, WAL mode, `STRICT` tables) plus one blob namespace. Hosted mode means one file per tenant.
- **Explicit context.** A `LibraryContext` is passed explicitly. No global singletons except the logger.
- **Concurrency.** One serialized writer connection (`BEGIN IMMEDIATE`) plus a pool of reader connections. Readers see consistent WAL snapshots.
- **Our own wrapper.** A small SQLite wrapper (no exceptions, ADR-0007) instead of an ORM. Repositories are concrete classes over a transaction.
- **Migrations.** Numbered SQL files applied in one transaction each, tracked with `PRAGMA user_version`. Downgrades are refused.

## Consequences

- Tenant isolation, backup, export and deletion are file operations: ideal for privacy and GDPR requests.
- No database server to run on the laptop.
- Hosted scale comes from sharding tenants across hub nodes and replicating with Litestream. We revisit this only with evidence.
- Cost: no cross-tenant analytics in SQL. Usage accounting is exported separately.

## Alternatives considered

- **PostgreSQL + pgvector.** Great for large multi-tenant SaaS, but a server to operate on every laptop. Rejected for now.
- **A single SQLite file with a `tenant_id` column.** Weaker isolation and harder deletion. Rejected.
