-- 0008: where a run's replicates start (plan P25, ADR-0019).
--
-- An A/A comparison measures the pipeline's run-to-run noise: the baseline
-- run again with nothing shared, its replicates drawing under other salts.
-- A run's replicate is its jobs' salt (ADR-0020), so the A/A run numbers
-- its replicates after the baseline's: `rreport eval run --label base-aa
-- --first-replicate 3` draws with salts 3, 4 and 5 where `base` drew with
-- 0, 1 and 2. Every run made before starts at 0, as it did.

ALTER TABLE eval_runs ADD COLUMN first_replicate INTEGER NOT NULL DEFAULT 0 CHECK (first_replicate >= 0);
