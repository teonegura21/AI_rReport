-- 0002: a job's memory of its model calls (ADR-0020). A resumed job finds
-- here the answer to every call it already made, and repeats none of them.
-- A job's rows are deleted once its results are committed: the artifacts are
-- the truth from then on (ADR-0004).

CREATE TABLE llm_memo (
    job_id        TEXT    NOT NULL,
    memo_key      TEXT    NOT NULL,  -- BLAKE3 of the request's key and the served model's checksum
    response      TEXT    NOT NULL,  -- the answer, as recordings keep it (rr::llm::to_json)
    created_at_ms INTEGER NOT NULL,
    PRIMARY KEY (job_id, memo_key)
) STRICT, WITHOUT ROWID;
