-- 0003: the battle log (ADR-0011). One row per mission of a job: how it
-- ended, which rung of the escalation ladder it reached and who answered,
-- and what it cost. Its calls, one row per attempt, are in llm_calls
-- (job_id, mission_id). A mission run again, by a resumed job, replaces its
-- row.
--
-- Times are Unix milliseconds (UTC). STRICT tables refuse values of the wrong
-- type instead of converting them.

CREATE TABLE missions (
    job_id               TEXT    NOT NULL,
    mission              TEXT    NOT NULL,  -- unique in its job
    kind                 TEXT    NOT NULL,  -- what it does: its calls' purpose
    prompt               TEXT,              -- the prompt and its version, "atomize@3"; NULL: none named
    role                 TEXT    NOT NULL,  -- the role it was ordered to
    started_at_ms        INTEGER NOT NULL,
    duration_ms          INTEGER NOT NULL CHECK (duration_ms >= 0),
    status               TEXT    NOT NULL,  -- 'ok', or the error class that stopped it (rr::Errc name)
    verdict              TEXT CHECK (verdict IN ('accepted', 'invalid', 'refused', 'ceiling')),
    error                TEXT,              -- the error, when status is not 'ok'
    step                 TEXT CHECK (step IN ('as_ordered', 'thinking', 'commander', 'other_family')),
    rungs                INTEGER NOT NULL CHECK (rungs >= 0),  -- 0: a ceiling came before any
    answered_by          TEXT,              -- the model whose answers were accepted
    samples              INTEGER NOT NULL CHECK (samples >= 1),
    accepted             INTEGER NOT NULL CHECK (accepted >= 0 AND accepted <= samples),
    calls                INTEGER NOT NULL CHECK (calls >= 0),
    prompt_tokens        INTEGER NOT NULL DEFAULT 0 CHECK (prompt_tokens >= 0),
    cached_prompt_tokens INTEGER NOT NULL DEFAULT 0 CHECK (cached_prompt_tokens >= 0),
    completion_tokens    INTEGER NOT NULL DEFAULT 0 CHECK (completion_tokens >= 0),
    problems             TEXT    NOT NULL DEFAULT '[]',  -- JSON array: what was wrong with the last answer that failed
    PRIMARY KEY (job_id, mission),
    CHECK ((status = 'ok') = (verdict IS NOT NULL)),
    CHECK ((status = 'ok') = (error IS NULL)),
    CHECK ((step IS NULL) = (rungs = 0))
) STRICT, WITHOUT ROWID;
