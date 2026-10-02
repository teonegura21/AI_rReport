-- 0007: the eval's judgements (plan P24, ADR-0019). A judgement reads an
-- article job's article with the eval's frozen judges (rr/eval/judgement.hpp).
-- It is a job of its own kind: resumable by its memo, cached by its stage's
-- key, and accounted apart, so that the judges' calls are never the article's
-- costs.
--
-- The table checks a job's kind, and SQLite cannot change a CHECK in place:
-- `jobs` is made anew with the kind added, its rows copied, the old table
-- dropped and the new one renamed (SQLite's procedure for what ALTER TABLE
-- cannot do). rr_db runs every migration with foreign keys off for this, and
-- checks every reference before it commits (rr/db/database.hpp): the rows
-- that refer to a job (its stage runs, its eval entries) refer to it still.

CREATE TABLE jobs_new (
    id            TEXT    PRIMARY KEY,  -- a ULID: jobs sort in the order created
    source        TEXT    NOT NULL,     -- a web address, a caption file's path, or the job judged
    tier          TEXT    NOT NULL,     -- fast | thorough | max (ADR-0018); a judgement's panel
    salt          INTEGER NOT NULL,     -- 0, or the eval's replicate (ADR-0020)
    status        TEXT    NOT NULL
                  CHECK (status IN ('running', 'done', 'failed', 'needs_user', 'stopped')),
    error         TEXT,                 -- why it is not done, as the user is told
    title         TEXT,                 -- the video's, once the source stage knows it
    elapsed_ms    INTEGER NOT NULL CHECK (elapsed_ms >= 0),  -- time spent running, across resumes
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL,
    kind          TEXT    NOT NULL DEFAULT 'article'
                  CHECK (kind IN ('article', 'annotation', 'judgement'))
) STRICT, WITHOUT ROWID;

INSERT INTO jobs_new (id, source, tier, salt, status, error, title, elapsed_ms, created_at_ms,
                      updated_at_ms, kind)
SELECT id, source, tier, salt, status, error, title, elapsed_ms, created_at_ms, updated_at_ms, kind
FROM jobs;

DROP TABLE jobs;
ALTER TABLE jobs_new RENAME TO jobs;

-- Which job judges an article job under a panel: one a panel, and an article
-- judged again by the same panel finds it (done: its scores; not: resumed).
CREATE TABLE eval_judgements (
    article_job_id TEXT NOT NULL REFERENCES jobs (id),
    panel          TEXT NOT NULL,                       -- "eval-v1"
    job_id         TEXT NOT NULL UNIQUE REFERENCES jobs (id),
    PRIMARY KEY (article_job_id, panel)
) STRICT, WITHOUT ROWID;
