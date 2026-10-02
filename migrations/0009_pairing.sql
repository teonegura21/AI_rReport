-- 0009: the eval's pairings (plan P25, ADR-0019). A pairing reads the
-- articles of one video in two runs with the eval's frozen judges, part by
-- part, each part in both orders, and says which reads better
-- (rr/eval/flow.hpp). As a judgement (0007), it is a job of its own kind:
-- resumable by its memo, cached by its stage's key, accounted apart.
--
-- The kind is checked by the table, which SQLite cannot change in place:
-- `jobs` is made anew, as 0007 made it, with the kind added.

CREATE TABLE jobs_new (
    id            TEXT    PRIMARY KEY,  -- a ULID: jobs sort in the order created
    source        TEXT    NOT NULL,     -- a web address, a caption file's path, the job judged, or the two paired
    tier          TEXT    NOT NULL,     -- fast | thorough | max (ADR-0018); a judgement's or pairing's panel
    salt          INTEGER NOT NULL,     -- 0, or the eval's replicate (ADR-0020)
    status        TEXT    NOT NULL
                  CHECK (status IN ('running', 'done', 'failed', 'needs_user', 'stopped')),
    error         TEXT,                 -- why it is not done, as the user is told
    title         TEXT,                 -- the video's, once the source stage knows it
    elapsed_ms    INTEGER NOT NULL CHECK (elapsed_ms >= 0),  -- time spent running, across resumes
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL,
    kind          TEXT    NOT NULL DEFAULT 'article'
                  CHECK (kind IN ('article', 'annotation', 'judgement', 'pairing'))
) STRICT, WITHOUT ROWID;

INSERT INTO jobs_new (id, source, tier, salt, status, error, title, elapsed_ms, created_at_ms,
                      updated_at_ms, kind)
SELECT id, source, tier, salt, status, error, title, elapsed_ms, created_at_ms, updated_at_ms, kind
FROM jobs;

DROP TABLE jobs;
ALTER TABLE jobs_new RENAME TO jobs;

-- Which job pairs two article jobs under a panel: one a pair a panel, in
-- the order compared (A, then B), and a pair compared again finds it.
CREATE TABLE eval_pairings (
    first_job_id  TEXT NOT NULL REFERENCES jobs (id),  -- A's article job
    second_job_id TEXT NOT NULL REFERENCES jobs (id),  -- B's
    panel         TEXT NOT NULL,                       -- "eval-v1"
    job_id        TEXT NOT NULL UNIQUE REFERENCES jobs (id),
    PRIMARY KEY (first_job_id, second_job_id, panel)
) STRICT, WITHOUT ROWID;
