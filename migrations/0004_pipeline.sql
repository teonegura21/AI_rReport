-- 0004: the pipeline's record (ADR-0004). A job takes a video through the
-- stages. Each stage's result is an artifact, kept once under the hash of its
-- content. The stage cache says which artifact answers a stage's key, so a
-- stage done once, by any job, is not done again for the same inputs and the
-- same setup. A stage's result, its cache entry and the job's record of it
-- commit in one transaction: a crash leaves all of them or none.
--
-- Times are Unix milliseconds (UTC). STRICT tables refuse values of the wrong
-- type instead of converting them.

-- What was asked for, and how far it got.
CREATE TABLE jobs (
    id            TEXT    PRIMARY KEY,  -- a ULID: jobs sort in the order created
    source        TEXT    NOT NULL,     -- a web address, or a caption file's path
    tier          TEXT    NOT NULL,     -- fast | thorough | max (ADR-0018)
    salt          INTEGER NOT NULL,     -- 0, or the eval's replicate (ADR-0020)
    status        TEXT    NOT NULL
                  CHECK (status IN ('running', 'done', 'failed', 'needs_user', 'stopped')),
    error         TEXT,                 -- why it is not done, as the user is told
    title         TEXT,                 -- the video's, once the source stage knows it
    elapsed_ms    INTEGER NOT NULL CHECK (elapsed_ms >= 0),  -- time spent running, across resumes
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL
) STRICT, WITHOUT ROWID;

-- Every stage result, once.
CREATE TABLE artifacts (
    hash          TEXT    PRIMARY KEY,  -- BLAKE3 of its canonical JSON (rr/json/json.hpp), in hex
    schema        TEXT    NOT NULL,     -- the schema it follows: "artifacts/transcript"
    codec         TEXT    NOT NULL,     -- how `body` holds it: "json", the canonical text itself
    body          BLOB    NOT NULL,
    bytes         INTEGER NOT NULL CHECK (bytes >= 0),  -- the canonical JSON's size
    created_at_ms INTEGER NOT NULL
) STRICT, WITHOUT ROWID;

-- Which artifact answers a stage's key (rr/pipeline/stage.hpp), and what the
-- stage could not do in making it: published, never hidden. A result that
-- depends on its job and not only on its inputs (the job's ceiling cut it
-- short, ADR-0018) is kept for that job alone, and has no entry here.
CREATE TABLE stage_cache (
    key           TEXT    PRIMARY KEY,
    stage         TEXT    NOT NULL,
    version       INTEGER NOT NULL,
    artifact      TEXT    NOT NULL REFERENCES artifacts (hash),
    findings      TEXT    NOT NULL,     -- a JSON list of messages
    created_at_ms INTEGER NOT NULL
) STRICT, WITHOUT ROWID;

-- Each stage of each job: done (perhaps by the cache), or failed. A stage run
-- again, by a resumed job, replaces its row.
CREATE TABLE stage_runs (
    job_id         TEXT    NOT NULL REFERENCES jobs (id),
    stage          TEXT    NOT NULL,
    key            TEXT    NOT NULL,    -- empty: it failed before its key was known
    status         TEXT    NOT NULL CHECK (status IN ('done', 'failed')),
    cached         INTEGER NOT NULL CHECK (cached IN (0, 1)),  -- 1: the stage cache answered
    artifact       TEXT    REFERENCES artifacts (hash),
    findings       TEXT    NOT NULL DEFAULT '[]',  -- a JSON list: what the stage could not do
    error          TEXT,                -- why it failed
    started_at_ms  INTEGER NOT NULL,
    finished_at_ms INTEGER NOT NULL,    -- by the wall clock, which may step back: never checked against started_at_ms
    PRIMARY KEY (job_id, stage),
    CHECK ((status = 'done') = (artifact IS NOT NULL)),
    CHECK ((status = 'failed') = (error IS NOT NULL)),
    CHECK (status = 'done' OR cached = 0)
) STRICT, WITHOUT ROWID;
