-- 0006: the eval's runs and their scores (plan P23, ADR-0019).
--
-- A run takes the eval set's videos through the article stages at one tier,
-- each a number of times (its replicates, each with its own salt: ADR-0020).
-- Its jobs are ordinary article jobs, made from the videos' snapshots: they
-- share the stage cache, the call log and the battle log with every other
-- job, and `rreport jobs` lists them. What is the eval's own is which job is
-- which video and replicate of which run, and the scores of each.

CREATE TABLE eval_runs (
    id            TEXT    PRIMARY KEY,          -- a ULID
    label         TEXT    NOT NULL UNIQUE,      -- what compare and gate name it by
    eval_set      TEXT    NOT NULL,             -- the set's name: "m2"
    split         TEXT    NOT NULL CHECK (split IN ('dev', 'test')),
    tier          TEXT    NOT NULL,             -- fast | thorough | max
    replicates    INTEGER NOT NULL CHECK (replicates >= 1),
    created_at_ms INTEGER NOT NULL
) STRICT, WITHOUT ROWID;

-- Which job is which video and replicate of a run.
CREATE TABLE eval_jobs (
    run_id    TEXT    NOT NULL REFERENCES eval_runs (id),
    video     TEXT    NOT NULL,                 -- the set's id for it
    replicate INTEGER NOT NULL CHECK (replicate >= 0),  -- and the job's salt
    job_id    TEXT    NOT NULL REFERENCES jobs (id),
    PRIMARY KEY (run_id, video, replicate)
) STRICT, WITHOUT ROWID;

-- A job's scores: one value a metric, all replaced when the job is scored
-- again (so no metric of an older scorer's definition outlives it).
CREATE TABLE eval_scores (
    run_id    TEXT    NOT NULL,
    video     TEXT    NOT NULL,
    replicate INTEGER NOT NULL,
    metric    TEXT    NOT NULL,                 -- "atoms_told", "minutes", …
    value     REAL    NOT NULL,
    scorer    TEXT    NOT NULL,                 -- what computed it, and its version: "code@1"
    PRIMARY KEY (run_id, video, replicate, metric),
    FOREIGN KEY (run_id, video, replicate) REFERENCES eval_jobs (run_id, video, replicate)
) STRICT, WITHOUT ROWID;
