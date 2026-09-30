-- 0001: accounting. Every attempt at a model call (plan §6, "Accounting").
--
-- Times are Unix milliseconds (UTC). STRICT tables refuse values of the wrong
-- type instead of converting them.

CREATE TABLE llm_calls (
    id                   TEXT    PRIMARY KEY,  -- a ULID, so ids sort by start time
    started_at_ms        INTEGER NOT NULL,
    duration_ms          INTEGER NOT NULL CHECK (duration_ms >= 0),
    first_token_ms       INTEGER CHECK (first_token_ms >= 0),  -- NULL: no token arrived
    provider             TEXT    NOT NULL,
    model                TEXT    NOT NULL,
    purpose              TEXT    NOT NULL,
    job_id               TEXT,                 -- NULL outside a job
    mission_id           TEXT,                 -- NULL outside a mission
    request_key          TEXT    NOT NULL,     -- attempts of one call share it
    status               TEXT    NOT NULL,     -- 'ok', or the error class (rr::Errc name)
    error                TEXT,                 -- the error's message, when status is not 'ok'
    finish_reason        TEXT,                 -- stop, length, content_filter, other; NULL after a failure
    prompt_tokens        INTEGER NOT NULL DEFAULT 0 CHECK (prompt_tokens >= 0),
    cached_prompt_tokens INTEGER NOT NULL DEFAULT 0 CHECK (cached_prompt_tokens >= 0),
    completion_tokens    INTEGER NOT NULL DEFAULT 0 CHECK (completion_tokens >= 0),
    CHECK ((status = 'ok') = (error IS NULL))
) STRICT;

CREATE INDEX llm_calls_by_start ON llm_calls (started_at_ms);
CREATE INDEX llm_calls_by_job ON llm_calls (job_id) WHERE job_id IS NOT NULL;
CREATE INDEX llm_calls_by_request ON llm_calls (request_key);

-- What was sent and received, kept apart from the numbers: a retention policy
-- deletes payloads and keeps the accounting.
CREATE TABLE llm_call_payloads (
    call_id  TEXT PRIMARY KEY REFERENCES llm_calls (id) ON DELETE CASCADE,
    request  TEXT NOT NULL,  -- canonical JSON of the request (rr::llm::to_json)
    response TEXT NOT NULL   -- {"content": …, "reasoning": …}; {} after a failure
) STRICT;
